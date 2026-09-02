/* mini-os/v2-c-kernel/src/app/tcp.h
 * 虚拟 TCP 薄包装（netif Step 4）——用户态库，建立在 sys_net_* (netsock UDP) 之上。
 * API 契约见 docs/tcp-thin-api.md；wire 见 docs/tcp-session-proto.md。
 * "薄"：guest 侧不实现 TCP 状态机；真实 TCP 由宿主转发器完成；上行只给
 * "状态可见"面（连接对象 + 事件通道），厚包装时在同一对象/通道补状态机即可。
 */
#ifndef APP_TCP_H
#define APP_TCP_H
#include <stdint.h>

#define TCP_CONN_MAX 4        /* 连接对象表容量（fd = 下标） */
#define TCP_EVQ      4        /* 每连接状态事件队列深度（DATA 不走此队，见 tcp.c） */
#define TCP_RXB      16384    /* 每连接数据接收环（v1.2 BUG-047：4096->16384，
                                drain 改单报泵取 + 应用边收边排空，大响应尾部不丢） */
#define TCP_DGRAM_BUF 1400    /* drain 单数据报缓冲（= netsock sendto/recvfrom 钳制上限，
                                含 8B 会话头；接收侧最大可交付=该值） */
#define TCP_MTU      1400     /* 发送硬墙：单数据报（含 8B 会话头）上限，与 netsock sendto
                                钳制一致（见 tcp-mtu-fail v1.1 收尾）；应用数据≤TCP_MAX_PAYLOAD */
#define TCP_RECV_TICKS 500    /* recv 阻塞超时上限（100Hz * 5s = 500 tick） */

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