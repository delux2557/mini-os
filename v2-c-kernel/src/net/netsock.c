/* mini-os/v2-c-kernel/src/net/netsock.c
 * 用户态 UDP socket（v0.20）：内核 UDP socket 表 + 轮询分发。
 *  - netsock_open：分配 socket 槽并绑定本地端口（0=自动分配）
 *  - netsock_send：经 SLIRP 网关 MAC 构建完整帧（Ethernet+IPv4+UDP）-> e1000_tx
 *  - netsock_recv：先"排空"网卡（取走当前已到帧，匹配本地端口的 UDP 数据报入队），
 *    再从本 socket 队列取队首数据报；无包返回 0（非阻塞，与轮询驱动一致）
 * 数据报载荷在 socket 表内排队，天然做到"进程多次 recv 之间 NIC 缓冲不丢"。
 */
#include "netsock.h"
#include "e1000.h"
#include "udp.h"
#include "dhcp.h"
#include "mem.h"
#include "sched.h"     /* v0.31 socket 归属：sched_current_pid */
#include "serial.h"
#include <stdint.h>

#define MY_IP 0x0A00020Fu   /* 10.0.2.15：SLIRP 分配给客户机的地址 */

static net_sock_t socks[NET_SOCK_MAX];
static uint16_t auto_port = 21000;   /* 自动分配端口起点（避开常用端口） */

/* v0.28 DHCP 租期续约接收端点：端口 68 专用 socket（见 netsock.h 说明） */
static int dhcp_sock = -1;

/* e1000 的 MMIO 位于高地址（0xfeb8xxxx，PDE>=512），而 addr_space_create 只
 * 克隆低 1GB PDE（dir[0..511]）给进程页目录。syscall 路径（CR3=当前进程页目录）
 * 直接访问设备寄存器会缺页；内核自检在启动期（CR3=内核页目录）则无需切换。
 * 故凡涉及 e1000 收发（MMIO 访问）都须临时切到内核页目录，用完切回。 */
static void enter_kernel_pd(uint32_t *saved) {
    *saved = mem_current_pd();
    switch_page_dir(mem_kernel_pd());
}
static void exit_kernel_pd(uint32_t saved) { switch_page_dir(saved); }

static net_sock_t *find_by_port(uint16_t port) {
    for (int i = 0; i < NET_SOCK_MAX; i++)
        if (socks[i].used && socks[i].local_port == port) return &socks[i];
    return 0;
}

/* 收一帧并分发：仅接受匹配某 socket 本地端口的 UDP 数据报，入对应队列 */
static void dispatch_frame(const uint8_t *frame, uint32_t len) {
    uint32_t sip = 0, plen = 0;
    uint16_t sp = 0, dp = 0;
    const uint8_t *pay = 0;
    if (udp_parse(frame, len, &sip, &sp, &dp, &pay, &plen) != 0) return;
    net_sock_t *s = find_by_port(dp);
    if (!s) return;
    if (plen > NET_RXMAX) plen = NET_RXMAX;
    uint32_t tail = s->rx_tail;
    uint32_t next = (tail + 1) % NET_RXQ;
    if (next == s->rx_head) return;               /* 队列满，丢包 */
    for (uint32_t i = 0; i < plen; i++) s->rxb[tail][i] = pay[i];
    s->rxsip[tail]   = sip;
    s->rxsport[tail] = sp;
    s->rxlen[tail]   = (uint16_t)plen;
    s->rx_tail = next;
}

/* 排空网卡：把当前已到达的帧全部取出并分发（每次 recv 前调用一次） */
static void netsock_drain(void) {
    uint8_t f[1600];
    uint32_t l;
    while (e1000_rx(f, sizeof(f), &l) == 1) dispatch_frame(f, l);
}

int netsock_open(uint16_t port) {
    int slot = -1;
    for (int i = 0; i < NET_SOCK_MAX; i++)
        if (!socks[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    if (port == 0) {
        for (int tries = 0; tries < 100; tries++) {
            uint16_t p = auto_port++;
            if (!find_by_port(p)) { port = p; break; }
        }
        if (port == 0) return -1;
    } else if (find_by_port(port)) {
        return -1;                                /* 端口占用 */
    }
    socks[slot].used       = 1;
    socks[slot].pid        = sched_current_pid();   /* v0.31 归属：本 socket 记打开者 */
    socks[slot].reserved   = 0;
    socks[slot].local_port = port;
    socks[slot].local_ip   = MY_IP;
    socks[slot].rx_head    = 0;
    socks[slot].rx_tail    = 0;
    serial_printf("[netsock] open id=%d port=%u (pid=%u)\n", slot, port, socks[slot].pid);
    return slot;
}

int netsock_send(int id, uint32_t dst_ip, uint16_t dst_port,
                 const uint8_t *data, uint32_t len) {
    if (id < 0 || id >= NET_SOCK_MAX || !socks[id].used) return -1;
    const uint8_t *gw = e1000_gw_mac();
    if (!gw) return -1;
    uint8_t frame[1600];
    uint32_t flen = udp_build_frame(frame, gw, e1000_mac(),
                                    socks[id].local_ip, dst_ip,
                                    socks[id].local_port, dst_port, data, len);
    uint32_t saved;
    enter_kernel_pd(&saved);            /* MMIO 访问须在内核页目录下 */
    int rc = e1000_tx(frame, flen);
    exit_kernel_pd(saved);
    if (rc < 0) return -1;
    return (int)len;
}

int netsock_recv(int id, uint8_t *buf, uint32_t max,
                 uint32_t *src_ip, uint16_t *src_port) {
    if (id < 0 || id >= NET_SOCK_MAX || !socks[id].used) return -1;
    uint32_t saved;
    enter_kernel_pd(&saved);            /* 排空网卡（e1000_rx 访问 MMIO） */
    netsock_drain();
    exit_kernel_pd(saved);
    if (socks[id].rx_head == socks[id].rx_tail) return 0;   /* 无包 */
    uint32_t h = socks[id].rx_head;
    uint32_t n = socks[id].rxlen[h];
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) buf[i] = socks[id].rxb[h][i];
    if (src_ip)   *src_ip   = socks[id].rxsip[h];
    if (src_port) *src_port = socks[id].rxsport[h];
    socks[id].rx_head = (h + 1) % NET_RXQ;
    return (int)n;
}

void netsock_close(int id) {
    if (id < 0 || id >= NET_SOCK_MAX) return;
    socks[id].used = 0;
    serial_printf("[netsock] close id=%d\n", id);
}

/* v0.31（F-0b）：仅当 socket 归 pid 所有且非内核保留槽时才关闭。
 * 防任意 ring3 程序 close(id=0) 打死 DHCP 续约端点。返回 0 已关 / -1 被拒。 */
int netsock_close_if_owner(int id, uint32_t pid) {
    if (id < 0 || id >= NET_SOCK_MAX || !socks[id].used) return -1;
    if (socks[id].reserved || socks[id].pid != pid) {
        serial_printf("[netsock] close id=%d DENIED (reserved=%d or not owner pid=%u)\n",
                      id, socks[id].reserved, pid);
        return -1;
    }
    netsock_close(id);
    return 0;
}

/* v0.31（F-0a）：进程退出时归还其打开的所有 socket（跳内核保留槽）。
 * 根治"开 socket 不关即退出 -> 槽位永久失踪，直到表满网络降级"。 */
void netsock_close_pid(uint32_t pid) {
    for (int i = 0; i < NET_SOCK_MAX; i++)
        if (socks[i].used && !socks[i].reserved && socks[i].pid == pid) {
            socks[i].used = 0;
            serial_printf("[netsock] close id=%d (proc %u exit cleanup)\n", i, pid);
        }
}

/* v0.31（F-0c）：socket 表审计——占用计数与数据库一致即健康（并入 kern_audit）。 */
uint32_t netsock_audit(void) {
    uint32_t used = 0;
    for (int i = 0; i < NET_SOCK_MAX; i++)
        if (socks[i].used) used++;
    serial_printf("[audit] netsock ok: used=%u/%d (dhcp_sock id=%d)\n",
                  used, NET_SOCK_MAX, dhcp_sock);
    return 0;
}

/* ---- v0.28 DHCP 租期续约接收端点 ----
 * 问题：用户 socket 的 recvfrom 会"排空"网卡（netsock_drain 取走 NIC 环所有帧），
 * 无匹配本地端口的帧（DHCP 应答 67->68）被直接丢弃——续约应答会被 sockdemo 等
 * 抢先消费。注册端口 68 的 DHCP socket，dispatch_frame 即把应答入其队列。 */
void netsock_dhcp_open(void) {
    if (dhcp_sock >= 0) return;
    dhcp_sock = netsock_open(DHCP_CLIENT_PORT);
    if (dhcp_sock >= 0) {
        socks[dhcp_sock].reserved = 1;   /* v0.31：内核保留槽，用户永不 close */
        socks[dhcp_sock].pid      = 0;
        serial_printf("[netsock] DHCP socket id=%d reserved (port %u)\n",
                      dhcp_sock, DHCP_CLIENT_PORT);
    }
}

/* 排空网卡并取一条 DHCP 应答载荷（BOOTP 内容，可直接交 dhcp_parse_reply）；
 * 返回载荷长度；0=无；-1=失败。非阻塞（与轮询驱动一致）。 */
int netsock_dhcp_recv(uint8_t *buf, uint32_t max) {
    if (dhcp_sock < 0) netsock_dhcp_open();
    if (dhcp_sock < 0) return -1;
    uint32_t saved;
    enter_kernel_pd(&saved);            /* 排空网卡（e1000_rx 访问 MMIO） */
    netsock_drain();
    exit_kernel_pd(saved);
    if (socks[dhcp_sock].rx_head == socks[dhcp_sock].rx_tail) return 0;
    uint32_t h = socks[dhcp_sock].rx_head;
    uint32_t n = socks[dhcp_sock].rxlen[h];
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) buf[i] = socks[dhcp_sock].rxb[h][i];
    socks[dhcp_sock].rx_head = (h + 1) % NET_RXQ;
    return (int)n;
}
