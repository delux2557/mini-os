/* mini-os/v2-c-kernel/src/net/netsock.h
 * 用户态 UDP socket（v0.20）：内核维护的 UDP socket 表 + 网卡轮询分发。
 * 极简、非阻塞：recvfrom 先"排空"网卡一次（把匹配本 socket 本地端口的 UDP
 * 数据报入队），再取队首返回；无包返回 0。与轮询式 e1000 驱动对齐。 */
#ifndef NET_NETSOCK_H
#define NET_NETSOCK_H
#include <stdint.h>

#define NET_SOCK_MAX 4      /* 内核 socket 表容量 */
#define NET_RXMAX    512    /* 单个数据报载荷上限 */
#define NET_RXQ      4      /* 每 socket 待收队列深度 */

typedef struct {
    int      used;
    uint16_t local_port;            /* 本地绑定端口 */
    uint32_t local_ip;              /* 固定 10.0.2.15 */
    uint8_t  rxb[NET_RXQ][NET_RXMAX];   /* 待收数据报载荷 */
    uint32_t rxsip[NET_RXQ];        /* 源 IP */
    uint16_t rxsport[NET_RXQ];      /* 源端口 */
    uint16_t rxlen[NET_RXQ];        /* 载荷长度 */
    uint32_t rx_head, rx_tail;      /* 环形队列：head=读(队首) tail=写(队尾) */
} net_sock_t;

/* 打开 UDP socket：port=0 自动分配；返回 socket id（0..NET_SOCK_MAX-1）或 -1 */
int  netsock_open(uint16_t port);
/* 发送：构建完整以太网帧（经 SLIRP 网关 MAC 寻址）-> e1000_tx；成功返回 len */
int  netsock_send(int id, uint32_t dst_ip, uint16_t dst_port,
                  const uint8_t *data, uint32_t len);
/* 接收：先排空网卡并分发，再取队首数据报拷入 buf（src_ip/src_port 出参）；
 * 返回载荷长度；0=无包；-1=非法 socket */
int  netsock_recv(int id, uint8_t *buf, uint32_t max,
                  uint32_t *src_ip, uint16_t *src_port);
void netsock_close(int id);

/* ---- v0.28 DHCP 租期续约接收端点（端口 68 专用 socket）----
 * 用户 socket 的 recvfrom 会"排空"网卡（取走 NIC 环所有帧），无匹配本地端口的帧
 * （如 DHCP 应答）会被丢弃——续约应答可能被抢先消费。注册端口 68 的 DHCP socket，
 * 让分发路径把 DHCP 应答入其队列，续约 tick 经 netsock_dhcp_recv 读取（与用户
 * socket 共享同一分发路径）。 */
void netsock_dhcp_open(void);   /* 打开 DHCP 客户端 socket（幂等；引导期 DHCP 前调用） */
int  netsock_dhcp_recv(uint8_t *buf, uint32_t max);  /* 排空网卡并取一条 DHCP 应答载荷 */

#endif /* NET_NETSOCK_H */
