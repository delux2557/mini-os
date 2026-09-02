/* mini-os/v2-c-kernel/src/app/tcp.h
 * 虚拟 TCP 薄包装（netif Step 4）——用户态库，建立在 sys_net_* (netsock UDP) 之上。
 * API 契约见 docs/tcp-thin-api.md；wire 见 docs/tcp-session-proto.md。
 * "薄"：guest 侧不实现 TCP 状态机；真实 TCP 由宿主转发器完成；上行只给
 * "状态可见"面（连接对象 + 事件通道），厚包装时在同一对象/通道补状态机即可。
 */
#ifndef APP_TCP_H
#define APP_TCP_H
#include <stdint.h>
#include "tcp_proto.h"      /* TCP_PHDR：会话协议头定长（tcp.h 用其推单报 payload 上限） */

#define TCP_CONN_MAX 4        /* 连接对象表容量（fd = 下标） */
#define TCP_EVQ      4        /* 每连接状态事件队列深度（DATA 不走此队，见 tcp.c） */
#define TCP_RXB      16384    /* 每连接数据接收环（v1.2 BUG-047：4096->16384，
                                drain 改单报泵取 + 应用边收边排空，大响应尾部不丢） */
#define TCP_DGRAM_BUF 1400    /* drain 单数据报缓冲（= netsock sendto/recvfrom 钳制上限，
                                含 8B 会话头；接收侧最大可交付=该值） */
#define TCP_MTU      1400     /* 发送硬墙：单数据报（含 8B 会话头）上限，与 netsock sendto
                                钳制一致（见 tcp-mtu-fail v1.1 收尾）；应用数据≤TCP_MAX_PAYLOAD */
#define TCP_MAX_PAYLOAD (TCP_MTU - TCP_PHDR)   /* 单报应用数据上限（单条上行载荷副本大小） */
#define TCP_RECV_TICKS 500    /* recv/发送让步阻塞超时上限（100Hz * 5s = 500 tick） */
#define TCP_TX_TICKS   250    /* 上行超时重传间隔（tick，100Hz*2.5s=250）：须 ≥ 慢通道单报回环 */
#define TCP_TXWIN      8      /* v1.3 上行滑动窗口：最多 W 个数据报同时在途（未确认） */

/* 运行期发送槽：保存一个已发送、尚未被累计 ACK 确收的上行数据报载荷副本（重传依据）。
   seq = 该载荷的上行序列号；len = 载荷字节数；tick = 最近一次发送/重传时刻。 */
typedef struct {
    uint16_t seq;
    uint16_t len;
    uint32_t tick;
    uint8_t  busy;            /* 槽在途（seq ∈ [tx_base, tx_seq)） */
    uint8_t  data[TCP_MAX_PAYLOAD];
} tx_slot_t;

/* 连接对象（docs/tcp-thin-api.md §2；本实现为用户态 per-process，故不含内核 pid/内核栈缓冲） */
typedef enum {
    TCP_FREE = 0, TCP_OPENING, TCP_OPEN, TCP_CLOSED, TCP_ERROR
} tcp_state_t;

typedef struct {
    int          used;
    uint32_t     session_id;    /* guest 分配，单调递增 */
    tcp_state_t  state;         /* FREE->OPENING->OPEN->CLOSED/ERROR */
    uint32_t     dst_ip;        /* open 目标（仅薄包装侧语义记录；wire 只在 MSG_OPEN 传一次） */
    uint16_t     dst_port;
    uint8_t      rxb[TCP_RXB];  uint16_t rx_head, rx_tail;
    uint16_t     rx_next;       /* v1.2 可靠下行：下一个期望的数据序列号 seq（stop-and-wait） */
    /* v1.3 上行滑动窗口：guest 是发送方，保留至多 TCP_TXWIN 个在途载荷副本（各带独立 seq），
       发满窗口即让步等累计 ACK（host→guest MSG_ACK = 下一期望上行 seq）推进 tx_base；
       最老未确认槽每 TCP_TX_TICKS 超时重传（幂等，转发器遇重复/乱序 seq 丢弃并回累计 ACK）。
       in-flight = tx_seq - tx_base（seq 单调，滑动窗口内恒 ≤ TCP_TXWIN）。 */
    uint16_t     tx_seq;        /* 下一个要分配的上行 DATA 序列号（分配递增） */
    uint16_t     tx_base;       /* 最老未确认 seq（= 已累计确认的边界；ACK 下一期望即推进到此） */
    uint32_t     tx_pending_start; /* 本连接最后入窗时刻（tcp_send 让步等待的防挂死基准） */
    tx_slot_t    tx_win[TCP_TXWIN]; /* 发送窗口（环，槽下标 = seq % TCP_TXWIN） */
    /* 状态事件队列（DATA 直接进 rxb，不占此队；状态事件绝不丢弃，见 spec §3） */
    struct { uint8_t type; } ev[TCP_EVQ];
    uint16_t     ev_head, ev_tail;
    uint8_t      ev_overflow;
} tcp_conn_t;

/* returns: >=0 fd / -1 本地失败 */
int  tcp_open(uint32_t ip, uint16_t port);
/* returns: 0 已建立（OPENED）/ -1 失败或超时（ERROR/TIMEOUT/超时上限） */
int  tcp_wait_open(int fd);
/* returns: >0 载荷已发 / -1 本地失败（无效 fd / 未建立 / 超单包上限 / 已关闭） */
int  tcp_send(int fd, const uint8_t *d, uint32_t n);
/* returns: >0 收到字节 / 0 对端正常关闭 / -1 失败或超时（三态互斥，见 spec §1.1） */
int  tcp_recv(int fd, uint8_t *buf, uint32_t max);
/* returns: 0 已关闭 / -1 无效 fd 或已关 */
int  tcp_close(int fd);

#endif /* APP_TCP_H */