/* mini-os/v2-c-kernel/src/drv/e1000.c
 * Intel 82540EM (e1000) 驱动（v0.18/v0.19）：QEMU 默认网卡（`-device e1000`）。
 *  - MMIO BAR0（128KB）经 pci_bar_alloc_mem 分配后恒等映射进内核页目录
 *  - legacy 16B 描述符环（RX 16 / TX 8），轮询（无中断）
 *  - 链路：软复位后置 CTRL.SLU，轮询 STATUS.LU
 *  - e1000_selftest()：发 ARP 请求（who has 10.0.2.2）收 SLIRP 回复，
 *    端到端验证 TX+RX（配合 QEMU filter-dump 的 pcap 作独立核验），并把
 *    网关 MAC 缓存供上层使用
 *  - e1000_udp_selftest()（v0.19）：经 SLIRP 网关回环，向宿主 UDP echo 服务
 *    发 "PING" 收 "PONG"（宿主 127.0.0.1:7777，测试脚本提供 echo server）
 *  - e1000_icmp_selftest()（v0.23）：发 ICMP Echo 请求到 SLIRP 网关 10.0.2.2，
 *    收其 Echo 应答（SLIRP 内置 ICMP 回显），即"PING 通宿主"的经典语义
 */
#include "e1000.h"
#include "pci.h"
#include "mem.h"
#include "serial.h"
#include "timer.h"
#include "netutil.h"
#include "ip.h"
#include "udp.h"
#include "icmp.h"
#include "dhcp.h"
#include <stdint.h>

#define E1000_VENDOR 0x8086u
#define E1000_DEVICE 0x100Eu   /* 82540EM */

#define REG_CTRL   0x0000u
#define REG_STATUS 0x0008u
#define REG_RCTL   0x0100u
#define REG_TCTL   0x0400u
#define REG_TIPG   0x0410u
#define REG_RDBAL  0x2800u
#define REG_RDBAH  0x2804u
#define REG_RDLEN  0x2808u
#define REG_RDH    0x2810u
#define REG_RDT    0x2818u
#define REG_TDBAL  0x3800u
#define REG_TDBAH  0x3804u
#define REG_TDLEN  0x3808u
#define REG_TDH    0x3810u
#define REG_TDT    0x3818u
#define REG_RAL0   0x5400u
#define REG_RAH0   0x5404u

#define CTRL_RST  (1u << 26)
#define CTRL_SLU  (1u << 6)
#define STATUS_LU (1u << 1)
#define RCTL_EN   (1u << 1)      /* 注意：e1000 的 EN 是 bit1（不是 bit0） */
#define RCTL_BAM  (1u << 15)     /* 接受广播 */
#define RCTL_SECRC (1u << 26)    /* 剥离以太网 CRC */
#define TCTL_EN   (1u << 1)      /* 同上，EN=bit1 */
#define TCTL_PSP  (1u << 3)      /* 短包自动填充到 60 字节 */

#define MMIO_SIZE 0x20000u       /* 128KB */
#define RX_N  16
#define TX_N  8
#define BUF_SIZE 2048

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;     /* bit0 = DD */
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;        /* bit0 EOP, bit3 IFCS, bit4 RS */
    uint8_t  status;     /* bit0 = DD */
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static volatile uint32_t *regs;
/* 描述符环必须 volatile：设备异步写 status(DD) 位，编译器不得把轮询读提升/缓存 */
static volatile struct e1000_rx_desc rx_ring[RX_N] __attribute__((aligned(4096)));
static volatile struct e1000_tx_desc tx_ring[TX_N] __attribute__((aligned(4096)));
static uint8_t rx_buf[RX_N][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buf[TX_N][BUF_SIZE] __attribute__((aligned(16)));
static uint32_t rx_tail = 0, tx_cur = 0;
static uint8_t mac[6];
static int ready = 0;
static uint8_t gw_mac[6];      /* 10.0.2.2 网关 MAC（ARP 自检学到） */
static int gw_known = 0;
/* v0.25：本机/网关 IP——DHCP 动态学得；失败保持静态兜底（单一配置点 NET_STATIC_*） */
static uint32_t my_ip = NET_STATIC_IP;
static uint32_t gw_ip = NET_STATIC_GW;

static inline uint32_t rd(uint32_t off) { return regs[off / 4]; }
static inline void wr(uint32_t off, uint32_t v) { regs[off / 4] = v; }

int e1000_init(void) {
    uint32_t bus, dev, func;
    if (!pci_find(E1000_VENDOR, E1000_DEVICE, &bus, &dev, &func)) {
        serial_puts("[net] e1000 not found on PCI\n");
        return -1;
    }
    uint32_t bar0 = pci_bar_alloc_mem(bus, dev, func, 0x10);
    if (!bar0) { serial_puts("[net] e1000 no MMIO BAR\n"); return -1; }
    regs = (volatile uint32_t *)bar0;

    /* 恒等映射 MMIO 到内核页目录（低半区 PDE 被所有进程克隆，全可见） */
    uint32_t pd = mem_kernel_pd();
    for (uint32_t off = 0; off < MMIO_SIZE; off += 4096)
        map_page_in(pd, bar0 + off, bar0 + off, 0x3);

    /* 软复位 -> 清复位 -> SLU 强制链路 */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (volatile uint32_t i = 0; i < 200000; i++) ;
    wr(REG_CTRL, rd(REG_CTRL) & ~CTRL_RST);
    for (volatile uint32_t i = 0; i < 200000; i++) ;
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);
    for (volatile uint32_t i = 0; i < 200000; i++) ;

    /* MAC：读 RAL0/RAH0（复位后由设备装入） */
    uint32_t ral = rd(REG_RAL0);
    uint32_t rah = rd(REG_RAH0);
    mac[0] = (uint8_t)ral;  mac[1] = (uint8_t)(ral >> 8);
    mac[2] = (uint8_t)(ral >> 16); mac[3] = (uint8_t)(ral >> 24);
    mac[4] = (uint8_t)rah;  mac[5] = (uint8_t)(rah >> 8);

    /* RX 环：预填缓冲地址；RDT=RX_N-1 把除尾部外的描述符交给硬件 */
    for (int i = 0; i < RX_N; i++) {
        rx_ring[i].addr = (uint64_t)(uint32_t)rx_buf[i];
        rx_ring[i].status = 0;
    }
    wr(REG_RDBAL, (uint32_t)rx_ring);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, RX_N * 16u);
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_N - 1);
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* 最后使能 */

    /* TX 环 */
    wr(REG_TDBAL, (uint32_t)tx_ring);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, TX_N * 16u);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TIPG, 0x0060200Au);   /* IPGT=10, IPGR1=8, IPGR2=6 */
    wr(REG_TCTL, TCTL_EN | TCTL_PSP);   /* 最后使能 */

    /* 等链路 */
    int link = 0;
    for (int i = 0; i < 3000000 && !link; i++)
        if (rd(REG_STATUS) & STATUS_LU) link = 1;

    ready = 1;
    serial_printf("[net] e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x bar=%x link=%d cmd=%x\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], bar0, link,
                  pci_config_read(bus, dev, func, 0x04));
    /* QEMU 特例：写 RCTL 会启动 1000ms 的 flush_queue_timer，此期间收到的包
     * 被排队、不进 RX 环。等它过期再收发，让自检一次通过（重试循环仍兜底）。 */
    serial_puts("[net] waiting for QEMU rx flush timer...\n");
    for (volatile uint32_t d = 0; d < 300000000u; d++) ;
    return 0;
}

int e1000_ready(void) { return ready ? 0 : -1; }
const uint8_t *e1000_mac(void) { return mac; }
const uint8_t *e1000_gw_mac(void) { return gw_known ? gw_mac : 0; }
uint32_t e1000_my_ip(void) { return my_ip; }
uint32_t e1000_gw_ip(void) { return gw_ip; }

/* 打印点分十进制 IP（大端序，直接按字节打印） */
static void print_ip(uint32_t ip) {
    serial_printf("%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16),
                  (unsigned)(ip >> 8), (unsigned)ip);
}

/* v0.25 DHCP 客户端：DISCOVER->OFFER->REQUEST->ACK 从 SLIRP DHCP 服务器动态获取
 * IP/网关；失败回退静态地址（NET_STATIC_IP/NET_STATIC_GW）。
 * 须在 timer_init 之前调用（本函数用忙等计时，不依赖 ticks）。
 * 轮询 RX：slirp 对 flags=0x8000 的广播请求回广播应答，e1000 RX 已开广播接收。 */
void e1000_dhcp_run(void) {
    if (!ready) { serial_puts("[dhcp] skipped (e1000 not ready)\n"); return; }
    uint32_t xid = 0x4D484350u | (uint32_t)mac[5];   /* 事务 ID（"MHCP"+MAC 末字节，非零即可） */
    uint8_t frame[512];
    int state = 0;                       /* 0:DISCOVER 1:REQUEST */
    uint32_t server_ip = 0, req_ip = 0;
    serial_puts("[dhcp] running...\n");

    for (int attempt = 0; attempt < 4; attempt++) {   /* 至多 4 轮收发（含超时重试） */
        /* 1) 按当前状态发一帧 */
        uint32_t flen = (state == 0)
            ? dhcp_build_discover(frame, mac, xid)
            : dhcp_build_request(frame, mac, xid, server_ip, req_ip);
        if (e1000_tx(frame, flen) < 0) { serial_puts("[dhcp] tx fail\n"); return; }
        serial_puts(state == 0 ? "[dhcp] sent DISCOVER\n" : "[dhcp] sent REQUEST\n");

        /* 2) 等应答：16 轮 x 忙等 ~125ms ≈ 2s 窗口 */
        int got = 0;
        for (int round = 0; round < 16 && !got; round++) {
            for (int i = 0; i < 6000; i++) {
                uint8_t rxb[1600]; uint32_t rlen = 0;
                if (e1000_rx(rxb, sizeof(rxb), &rlen) == 1) {
                    uint32_t sip = 0; uint16_t sp = 0, dp = 0;
                    const uint8_t *pay = 0; uint32_t plen = 0;
                    if (udp_parse(rxb, rlen, &sip, &sp, &dp, &pay, &plen) != 0 ||
                        sp != DHCP_SERVER_PORT || dp != DHCP_CLIENT_PORT)
                        continue;                        /* 只收 DHCP 67->68 */
                    uint8_t mt = 0; uint32_t yi = 0, si = 0, rt = 0, ls = 0;
                    if (dhcp_parse_reply(pay, plen, xid, &mt, &yi, &si, &rt, &ls) != 0)
                        continue;                        /* 非本事务的应答 */
                    if (mt == DHCP_MSG_OFFER) {
                        serial_puts("[dhcp] OFFER: ip ");
                        print_ip(yi);
                        serial_puts(", gw ");
                        print_ip(rt);
                        serial_printf(", lease %us\n", (unsigned)ls);
                        server_ip = si;
                        req_ip = yi;
                        state = 1;                       /* 下一轮发 REQUEST */
                        got = 1;
                    } else if (mt == DHCP_MSG_ACK) {
                        my_ip = yi;
                        gw_ip = rt;
                        serial_puts("[dhcp] ACK: ip ");
                        print_ip(my_ip);
                        serial_puts(", gw ");
                        print_ip(gw_ip);
                        serial_printf(", lease %us\n", (unsigned)ls);
                        return;                          /* DHCP 成功 */
                    } else if (mt == DHCP_MSG_NAK) {
                        serial_puts("[dhcp] NAK received\n");
                        state = 0;                       /* 回到 DISCOVER 重来 */
                    }
                }
                for (volatile uint32_t d = 0; d < 1000; d++) ;
            }
        }
        if (!got && state == 1) state = 0;               /* REQUEST 超时则从 DISCOVER 重来 */
    }
    serial_puts("[dhcp] failed, falling back to static ");
    print_ip(my_ip);
    serial_puts(" / ");
    print_ip(gw_ip);
    serial_puts("\n");
}

int e1000_tx(const uint8_t *data, uint32_t len) {
    if (!ready || len == 0 || len > BUF_SIZE) return -1;
    uint32_t idx = tx_cur % TX_N;
    volatile struct e1000_tx_desc *d = &tx_ring[idx];
    uint8_t *dst = tx_buf[idx];
    for (uint32_t i = 0; i < len; i++) dst[i] = data[i];
    d->addr   = (uint64_t)(uint32_t)dst;
    d->length = (uint16_t)len;
    d->cso    = 0;
    d->cmd    = 0x1Fu;          /* EOP|IFG|IC|IFCS|RS */
    d->status = 0;
    d->css    = 0;
    d->special = 0;
    __asm__ volatile ("" ::: "memory");
    tx_cur++;
    wr(REG_TDT, tx_cur % TX_N);   /* 尾指针 = 下一空闲槽（排他） */
    for (int i = 0; i < 3000000; i++)
        if (d->status & 1u) return 0;   /* 设备取走并发送完成 */
    serial_printf("[net] TX timeout: TDH=%x TDT=%x TCTL=%x status=%x\n",
                  rd(REG_TDH), rd(REG_TDT), rd(REG_TCTL), d->status);
    return -1;
}

int e1000_rx(uint8_t *buf, uint32_t max, uint32_t *len) {
    if (!ready) return -1;
    volatile struct e1000_rx_desc *d = &rx_ring[rx_tail];
    if (!(d->status & 1u)) return 0;          /* 无新包 */
    uint32_t n = d->length;
    if (n > max) n = max;
    const uint8_t *src = rx_buf[rx_tail];
    for (uint32_t i = 0; i < n; i++) buf[i] = src[i];
    d->status = 0;
    /* 归还刚消费的描述符给硬件：RDT = 该下标（写成 i+1 会让 RDH==RDT，
     * QEMU e1000_can_receive 判"无缓冲"而丢弃后续收包） */
    wr(REG_RDT, rx_tail);
    rx_tail = (rx_tail + 1) % RX_N;
    *len = n;
    return 1;
}

void e1000_selftest(void) {
    if (!ready) { serial_puts("[net] selftest skipped (no e1000)\n"); return; }
    uint32_t my_ip = e1000_my_ip();   /* v0.25：DHCP 学得或静态兜底 */
    uint32_t gw_ip = e1000_gw_ip();
    uint8_t frame[64];
    for (int attempt = 0; attempt < 5; attempt++) {
        int flen = net_build_arp_request(frame, mac, my_ip, gw_ip);
        if (e1000_tx(frame, (uint32_t)flen) < 0) { serial_puts("[net] selftest: tx fail\n"); return; }
        serial_printf("[net] selftest: tx ARP req (who has 10.0.2.2) #%d\n", attempt);
        for (int i = 0; i < 30000; i++) {
            uint8_t rxb[1600];
            uint32_t rlen = 0;
            if (e1000_rx(rxb, sizeof(rxb), &rlen) == 1) {
                uint32_t sip = 0;
                uint8_t smac[6];
                if (net_parse_arp_reply(rxb, rlen, &sip, smac) == 0) {
                    for (int j = 0; j < 6; j++) gw_mac[j] = smac[j];
                    gw_known = 1;
                    serial_printf("[net] selftest: rx ARP reply 10.0.2.2 @ "
                                  "%02x:%02x:%02x:%02x:%02x:%02x -> OK\n",
                                  smac[0], smac[1], smac[2], smac[3], smac[4], smac[5]);
                    return;
                }
            }
            for (volatile uint32_t d = 0; d < 2000; d++) ;
        }
    }
    serial_puts("[net] selftest: ARP exchange FAIL (no reply)\n");
}

void e1000_udp_selftest(void) {
    if (!ready || !gw_known) { serial_puts("[net] udp selftest skipped (no gw)\n"); return; }
    uint32_t my_ip = e1000_my_ip();   /* v0.25：DHCP 学得或静态兜底 */
    uint32_t gw_ip = e1000_gw_ip();   /* 10.0.2.2 -> 宿主 127.0.0.1（SLIRP 别名） */
    uint8_t frame[BUF_SIZE];
    /* 向宿主 UDP echo 服务（127.0.0.1:7777）发 PING；echo server 回 PONG+原载荷 */
    int flen = udp_build_frame(frame, gw_mac, mac, my_ip, gw_ip, 7777, 7777,
                               (const uint8_t *)"PING", 4);
    if (flen <= 0 || e1000_tx(frame, (uint32_t)flen) < 0) {
        serial_puts("[net] udp selftest: tx fail\n"); return;
    }
    serial_printf("[net] udp: tx %dB -> 10.0.2.2:7777 (PING)\n", flen);
    for (int attempt = 0; attempt < 5; attempt++) {
        for (int i = 0; i < 30000; i++) {
            uint8_t rxb[1600];
            uint32_t rlen = 0;
            if (e1000_rx(rxb, sizeof(rxb), &rlen) == 1) {
                uint32_t sip = 0; uint16_t sp = 0, dp = 0;
                const uint8_t *pay = 0; uint32_t plen = 0;
                if (udp_parse(rxb, rlen, &sip, &sp, &dp, &pay, &plen) == 0 &&
                    dp == 7777 && plen >= 4 && pay[0] == 'P' && pay[1] == 'O' &&
                    pay[2] == 'N' && pay[3] == 'G') {
                    serial_printf("[net] udp echo: rx %uB 'PONG' from 10.0.2.2:%u -> OK\n",
                                  plen, sp);
                    return;
                }
            }
            for (volatile uint32_t d = 0; d < 2000; d++) ;
        }
    }
    serial_puts("[net] udp echo FAIL (no PONG)\n");
}

void e1000_icmp_selftest(void) {
    if (!ready || !gw_known) { serial_puts("[net] icmp selftest skipped (no gw)\n"); return; }
    uint32_t my_ip = e1000_my_ip();   /* v0.25：DHCP 学得或静态兜底 */
    uint32_t gw_ip = e1000_gw_ip();   /* SLIRP 网关（内置 ICMP 回显） */
    uint8_t frame[BUF_SIZE];
    /* 发 Echo 请求（id=0x4242, seq=1, 载荷 "PING"）；SLIRP 回 Echo 应答（type=0） */
    int flen = icmp_build_frame(frame, gw_mac, mac, my_ip, gw_ip,
                                0x4242u, 1, (const uint8_t *)"PING", 4);
    if (flen <= 0 || e1000_tx(frame, (uint32_t)flen) < 0) {
        serial_puts("[net] icmp selftest: tx fail\n"); return;
    }
    serial_printf("[net] icmp: tx echo req (%dB) -> 10.0.2.2\n", flen);
    uint32_t t0 = (uint32_t)ticks;
    for (int attempt = 0; attempt < 5; attempt++) {
        for (int i = 0; i < 30000; i++) {
            uint8_t rxb[1600];
            uint32_t rlen = 0;
            if (e1000_rx(rxb, sizeof(rxb), &rlen) == 1) {
                uint32_t sip = 0;
                uint8_t type = 0, code = 0;
                uint16_t id = 0, seq = 0;
                const uint8_t *pay = 0; uint32_t plen = 0;
                if (icmp_parse(rxb, rlen, &sip, &type, &code, &id, &seq, &pay, &plen) == 0 &&
                    type == ICMP_TYPE_ECHO_REP && code == 0 &&
                    id == 0x4242u && seq == 1 && plen >= 4 &&
                    pay[0] == 'P' && pay[1] == 'I' && pay[2] == 'N' && pay[3] == 'G') {
                    serial_printf("[icmp] echo reply from 10.0.2.2 OK (rtt=%u ticks)\n",
                                  (uint32_t)ticks - t0);
                    return;
                }
            }
            for (volatile uint32_t d = 0; d < 2000; d++) ;
        }
    }
    serial_puts("[icmp] echo FAIL (no reply)\n");
}
