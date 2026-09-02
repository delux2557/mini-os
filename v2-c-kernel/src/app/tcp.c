/* mini-os/v2-c-kernel/src/app/tcp.c
 * 虚拟 TCP 薄包装实现（netif Step 4）。
 * 机制：单条 UDP socket（懒创建）承载全部会话，会话按 session_id 在载荷内多路复用，
 * 与宿主转发器（tests/tcp_proxy.py）经会话协议头交互：
 *   连接:  MSG_OPEN  (payload = dst_ip(4BE)+dst_port(2BE))   -> 宿主对目标发真实 TCP
 *   上传:  MSG_DATA  (payload = 应用字节)
 *   回传:  MSG_DATA(下行) + 事件 MSG_OPENED/CLOSED/ERROR/TIMEOUT
 *   注销:  MSG_CLOSE (payload 空)                          -> 宿主关 TCP、回收会话表
 * MTU 硬墙：单数据报（含 8B 会话头）上限 = TCP_MTU(1400)，与 netsock sendto/recvfrom
 * 钳制一致（e1000 1472/SLIP 1600 链路 MTU 名目由 1400 钳制涵盖）；
 * 超限由 tcp_send 本地返回 -1（见 docs/tcp-mtu-fail.md §2：guest 报错误码，不走转发器）。
 * recv 为有超时上限的阻塞等待（sys_sleep tick 轮询），三态返回 >0/0/-1 互斥。
 */
#include <stdint.h>
#include "user_lib.h"
#include "netio.h"
#include "tcp.h"
#include "tcp_proto.h"

#define TCP_PROXY_IP   0x0A000202u   /* 10.0.2.2：宿主转发器所在（SLIRP 网关 / 串口对端） */
#define TCP_PROXY_PORT 7778

static tcp_conn_t conns[TCP_CONN_MAX];
static int  tt_sock = -1;        /* 复用：唯一 UDP socket（懒创建） */
static uint32_t tt_sid = 0;      /* session_id 单调递增计数器（guest 分配） */

/* 向转发器发一条会话消息（可携带 flags/seq）；成功 0 / -1 */
static int sendpkt_f(uint32_t session_id, uint8_t type, uint16_t seq,
                     const uint8_t *pay, uint32_t plen) {
    uint8_t pkt[TCP_MTU];
    if (TCP_PHDR + plen > TCP_MTU) return -1;
    tcp_hdr_put_session(pkt, session_id);
    tcp_hdr_set_type(pkt, type, seq);
    for (uint32_t i = 0; i < plen; i++) pkt[TCP_PHDR + i] = pay[i];
    return sys_net_sendto(tt_sock, &(struct net_send_iov){
        .dst_ip = TCP_PROXY_IP, .dst_port = TCP_PROXY_PORT,
        .buf = pkt, .len = TCP_PHDR + plen });
}

/* 无 seq 消息（控制/事件，flags=0） */
static int sendpkt(uint32_t session_id, uint8_t type, const uint8_t *pay, uint32_t plen) {
    return sendpkt_f(session_id, type, 0, pay, plen);
}

/* v1.2 可靠下行：向转发器回送累计 ACK（payload = 下一个期望 seq，2BE）。 */
static int send_ack(tcp_conn_t *c) {
    uint8_t pay[2];
    pay[0] = (uint8_t)(c->rx_next >> 8);
    pay[1] = (uint8_t)c->rx_next;
    return sendpkt(c->session_id, MSG_ACK, pay, 2);
}

static tcp_conn_t *conn_by_session(uint32_t sid) {
    for (int i = 0; i < TCP_CONN_MAX; i++)
        if (conns[i].used && conns[i].session_id == sid) return &conns[i];
    return 0;
}

/* === v1.3 上行滑动窗口：发送侧状态辅助（guest=发送方） === */

/* 在途（未确认）包数 = tx_seq - tx_base。seq 单调递增且窗口内恒 ≤ TCP_TXWIN，差值取模 2^16 不溢出。 */
static uint16_t tx_inflight(tcp_conn_t *c) {
    return (uint16_t)(c->tx_seq - c->tx_base);
}

/* 超时重传"最老未确认槽"（头部）。每 TCP_TX_TICKS 只重发一个在途包并复位其 tick——
   SR 风格、最温和；幂等（转发器遇重复 seq 丢弃并回累计 ACK，接收侧自愈）。 */
static void tx_retrans(tcp_conn_t *c) {
    if (c->state != TCP_OPEN || tx_inflight(c) == 0) return;
    tx_slot_t *s = &c->tx_win[c->tx_base % TCP_TXWIN];
    if (s->busy && sys_getticks() - s->tick >= TCP_TX_TICKS) {
        sendpkt_f(c->session_id, MSG_DATA, c->tx_base, s->data, s->len);
        s->tick = sys_getticks();
    }
}

/* 累计 ACK 推进：next = 转发器"下一期望上行 seq" → 所有 seq < next 均已确收。
   仅当 next 严格落在 (tx_base, tx_seq] 内才推进（重复/越界 ACK 忽略），并清被确收槽 busy。 */
static void tx_ack(tcp_conn_t *c, uint16_t next) {
    uint16_t count = tx_inflight(c);
    uint16_t adv = (uint16_t)(next - c->tx_base);
    if (adv == 0 || adv > count) return;           /* 重复 ACK / 超出窗口：忽略 */
    for (uint16_t i = 0; i < adv; i++)
        c->tx_win[(c->tx_base + i) % TCP_TXWIN].busy = 0;
    c->tx_base = next;
}

/* 状态事件入队（spec §3：状态事件绝不丢弃；队满覆盖最旧，latest wins） */
static void ev_push(tcp_conn_t *c, uint8_t type) {
    c->ev[c->ev_tail].type = type;
    uint16_t next = (uint16_t)((c->ev_tail + 1) % TCP_EVQ);
    if (next == c->ev_head) {     /* 满：覆盖最旧，语义取最新 */
        c->ev_head = (uint16_t)((c->ev_head + 1) % TCP_EVQ);
        c->ev_overflow = 1;
    }
    c->ev_tail = next;
}

/* 数据事件：进入该连接的 rxb 流式缓冲（DATA 可牺牲，满即丢弃并置 overflow） */
static void rx_push(tcp_conn_t *c, const uint8_t *data, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint16_t next = (uint16_t)((c->rx_tail + 1) % TCP_RXB);
        if (next == c->rx_head) { c->ev_overflow = 1; return; }  /* 数据环满：丢弃 */
        c->rxb[c->rx_tail] = data[i];
        c->rx_tail = next;
    }
}

/* 泵取 UDP：从转发器回传队列只取"一个"会话数据报并路由进对应连接对象。
 * v1.2 BUG-047：原 for(;;) 全量 pump 把整段响应一次性挤进 rxb 环，超过 TCP_RXB 即丢尾；
 * 改为单报泵取，令 tcp_recv 每轮 drain 后立即排空，环永不涨破 TCP_RXB。
 * v1.2 可靠下行（stop-and-wait）：host→guest 的 MSG_DATA 携带序列号 seq，本函数
 * 只在 seq 恰为期望的 rx_next 时推入 rxb 并回 ACK，转发器收到 ACK 才发下一个；
 * 重复/乱序（ACK 丢后重发）直接丢弃并重发 ACK，保证大文件尾字节不丢。 */
static void drain(void) {
    uint8_t tmp[TCP_DGRAM_BUF];
    struct net_recv_iov ri;
    ri.src_ip = 0; ri.src_port = 0; ri._pad = 0;
    ri.buf = tmp; ri.max = sizeof(tmp);
    int n = sys_net_recvfrom(tt_sock, &ri);
    if (n <= 0) return;
    uint32_t sid; uint8_t mt;
    if (tcp_parse_hdr(tmp, (uint32_t)n, &sid, &mt) != 0) return;   /* 非法头丢弃 */
    tcp_conn_t *c = conn_by_session(sid);
    if (!c) return;
    if (mt == MSG_DATA) {
        uint16_t seq = tcp_hdr_get_seq(tmp);
        if (seq == c->rx_next) {
            rx_push(c, tmp + TCP_PHDR, (uint32_t)n - TCP_PHDR);
            c->rx_next++;
            send_ack(c);                 /* 累计 ACK：请继续发下一个 */
        } else {
            send_ack(c);                 /* 重发/乱序：丢弃载荷，重发 ACK 触发端对端自愈 */
        }
    }
    else if (mt == MSG_OPENED)  { c->state = TCP_OPEN; }
    else if (mt == MSG_CLOSED)  { c->state = TCP_CLOSED; ev_push(c, mt); }
    else if (mt == MSG_ERROR)   { c->state = TCP_ERROR;  ev_push(c, mt); }
    else if (mt == MSG_TIMEOUT) { c->state = TCP_ERROR;  ev_push(c, mt); }
    else if (mt == MSG_ACK)     { /* host→guest 的 MSG_ACK = 上行累计确认（payload 2B 下一期望上行 seq） */
        if (n >= TCP_PHDR + 2) {
            uint16_t next = (uint16_t)(((uint16_t)tmp[TCP_PHDR] << 8) | tmp[TCP_PHDR + 1]);
            tx_ack(c, next);     /* v1.3：累计推进 tx_base，清被确收槽 */
        }
    }
    /* MSG_OPEN/CLOSE 是 guest→host 方向，guest 收到即方向非法：忽略 */
}

int tcp_open(uint32_t ip, uint16_t port) {
    if (ip == 0 || port == 0) return -1;
    int slot = -1;
    for (int i = 0; i < TCP_CONN_MAX; i++) if (!conns[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    if (tt_sock < 0) {
        int s = sys_net_socket(0);       /* 自动分配本地 UDP 端口 */
        if (s < 0) return -1;
        tt_sock = s;
    }
    tcp_conn_t *c = &conns[slot];
    c->used = 1;
    c->session_id = ++tt_sid;
    c->state = TCP_OPENING;
    c->dst_ip = ip; c->dst_port = port;
    c->rx_head = c->rx_tail = c->ev_head = c->ev_tail = 0;
    c->rx_next = 0;                  /* v1.2 可靠下行：从 seq 0 开始期待 */
    c->tx_seq = 0; c->tx_base = 0; c->tx_pending_start = 0;  /* v1.3 上行滑动窗口：从 seq 0 起，窗口空 */
    /* tx_win 槽为静态零初始化（busy=0=空闲），连接复用前无需逐槽清零 */
    c->ev_overflow = 0;
    /* MSG_OPEN：把目标地址一次性带给转发器（dst_ip + dst_port，大端） */
    uint8_t pay[6];
    pay[0] = (uint8_t)(ip >> 24); pay[1] = (uint8_t)(ip >> 16);
    pay[2] = (uint8_t)(ip >> 8);  pay[3] = (uint8_t)ip;
    pay[4] = (uint8_t)(port >> 8); pay[5] = (uint8_t)port;
    if (sendpkt(c->session_id, MSG_OPEN, pay, 6) < 0) { c->used = 0; return -1; }
    return slot;
}

int tcp_send(int fd, const uint8_t *d, uint32_t n) {
    if (fd < 0 || fd >= TCP_CONN_MAX || !conns[fd].used) return -1;
    if (TCP_PHDR + n > TCP_MTU) return -1;            /* MTU 硬墙：本地早返（docs §2） */
    tcp_conn_t *c = &conns[fd];
    if (c->state != TCP_OPEN) return -1;              /* 未建立/已关/失败：本地立即拒绝 */
    /* === v1.3 上行滑动窗口：载荷写入发送窗口（分配上行 seq），有窗位即刻发出并返回；窗口满则
     * 让步等累计 ACK 推进 tx_base（期间 drain 收 ACK + 每 tick 驱动最老槽超时重传）。
     * 返回 n 即载荷已入窗并发出——可靠性由转发器累计确认 + 本函数让步期重传保证，调用方无需再等确收。
     * 大文件上传=循环 tcp_send：窗口带流水线，按扇出去即可无需逐包等 ACK。 === */
    uint32_t start = sys_getticks();
    for (;;) {
        drain();                                      /* 收上行 ACK 推进窗口 */
        if (c->state == TCP_CLOSED || c->state == TCP_ERROR) return -1;
        if (sys_getticks() - start >= TCP_RECV_TICKS) return -1;  /* 让步等待防挂死上限 */
        tx_retrans(c);                                /* 每 tick 驱动最老槽超时重传 */
        if (tx_inflight(c) >= TCP_TXWIN) { sys_sleep(1); continue; } /* 窗口满：让出等 ACK 腾位 */
        break;
    }
    tx_slot_t *s = &c->tx_win[c->tx_seq % TCP_TXWIN];
    for (uint32_t i = 0; i < n; i++) s->data[i] = d[i];
    s->len = (uint16_t)n; s->seq = c->tx_seq;
    s->busy = 1; s->tick = sys_getticks();
    if (sendpkt_f(c->session_id, MSG_DATA, c->tx_seq, s->data, s->len) < 0) return -1;
    c->tx_seq = (uint16_t)(c->tx_seq + 1);
    return (int)n;
}

int tcp_recv(int fd, uint8_t *buf, uint32_t max) {
    if (fd < 0 || fd >= TCP_CONN_MAX || !conns[fd].used) return -1;
    tcp_conn_t *c = &conns[fd];
    uint32_t start = sys_getticks();
    for (;;) {
        drain();
        /* 1) 数据优先：有载荷即返回 */
        if (c->rx_head != c->rx_tail) {
            uint32_t got = 0;
            while (c->rx_head != c->rx_tail && got < max) {
                buf[got++] = c->rxb[c->rx_head];
                c->rx_head = (uint16_t)((c->rx_head + 1) % TCP_RXB);
            }
            return (int)got;
        }
        /* 2) 状态事件：断开=0 / 失败超时=-1（与"无数据"恒可区分） */
        if (c->ev_head != c->ev_tail) {
            uint8_t t = c->ev[c->ev_head].type;
            c->ev_head = (uint16_t)((c->ev_head + 1) % TCP_EVQ);
            if (t == MSG_CLOSED)  return 0;   /* 对端正常关闭 */
            if (t == MSG_ERROR || t == MSG_TIMEOUT) return -1;
            continue;
        }
        /* 3) 超时上限防挂死 */
        if (sys_getticks() - start >= TCP_RECV_TICKS) return -1;
        tx_retrans(c);        /* v1.3：app 转入收读阶段仍驱动在途上行包超时重传 */
        sys_sleep(1);
    }
}

/* 等待连接建立：阻塞（有超时上限）直到 OPENED（state=OPEN 返回 0）或
 * ERROR/TIMEOUT（返回 -1）。真实连接握手在宿主，guest 只等事件。 */
int tcp_wait_open(int fd) {
    if (fd < 0 || fd >= TCP_CONN_MAX || !conns[fd].used) return -1;
    tcp_conn_t *c = &conns[fd];
    uint32_t start = sys_getticks();
    for (;;) {
        drain();
        if (c->state == TCP_OPEN) return 0;
        if (c->state == TCP_ERROR) return -1;
        if (c->ev_head != c->ev_tail) {
            uint8_t t = c->ev[c->ev_head].type;
            c->ev_head = (uint16_t)((c->ev_head + 1) % TCP_EVQ);
            if (t == MSG_ERROR || t == MSG_TIMEOUT) return -1;
        }
        if (sys_getticks() - start >= TCP_RECV_TICKS) return -1;
        tx_retrans(c);        /* v1.3：等待 OPENED 期间也驱动在途上行包重传 */
        sys_sleep(1);
    }
}

int tcp_close(int fd) {
    if (fd < 0 || fd >= TCP_CONN_MAX || !conns[fd].used) return -1;
    tcp_conn_t *c = &conns[fd];
    sendpkt(c->session_id, MSG_CLOSE, 0, 0);   /* 通知转发器注销（尽力而为） */
    c->used = 0;
    return 0;
}