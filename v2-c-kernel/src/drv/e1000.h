/* mini-os/v2-c-kernel/src/drv/e1000.h
 * Intel 82540EM (e1000) 驱动（v0.18）：QEMU 默认网卡，MMIO + 描述符环，纯轮询。
 */
#ifndef _E1000_H
#define _E1000_H
#include <stdint.h>

int e1000_init(void);                 /* 探测 + 初始化；成功返回 0 */
int e1000_ready(void);                /* 驱动是否就绪 */
const uint8_t *e1000_mac(void);       /* 6 字节 MAC */

/* v1.1 Step 1：把 e1000 注册为 netif 后端（netif 适配层在 src/drv/e1000_netif.c）。 */
void e1000_netif_register(void);

/* v0.20: 网关（10.0.2.2）MAC——ARP 自检学到；供上层 socket 发送寻址用；未学到返回 NULL */
const uint8_t *e1000_gw_mac(void);

/* 发一帧（数据自 data 拷入内部 TX 缓冲，等待设备取走后返回）；成功 0 */
int e1000_tx(const uint8_t *data, uint32_t len);

/* 收一帧：无包返回 0，收到则拷入 buf 并置 *len 返回 1，失败 -1 */
int e1000_rx(uint8_t *buf, uint32_t max, uint32_t *len);

/* 启动自检：发 ARP 请求（who has 10.0.2.2）并等待 SLIRP 回复，打印里程碑；
 * 学到网关 MAC 后缓存，供 e1000_udp_selftest 使用 */
void e1000_selftest(void);

/* v0.35（R1.2，外部审计 A1）：网关 ARP 学习独立为功能性路径——启动序列在
 * DHCP 之后调用，供一切外发 IPv4 帧寻址；selftest 仅作验证保留。返回 0=成功。 */
int e1000_arp_learn_gw(void);

/* v0.19 UDP 回环自检：向宿主 127.0.0.1:7777 的 echo 服务发 PING 收 PONG */
void e1000_udp_selftest(void);

/* v0.23 ICMP Echo 自检：发 Echo 请求到 SLIRP 网关 10.0.2.2，收其 Echo 应答（PING 通宿主） */
void e1000_icmp_selftest(void);

/* v0.25 DHCP 客户端：DISCOVER->OFFER->REQUEST->ACK 从 SLIRP DHCP 服务器动态获取
 * IP/网关；失败回退静态地址（NET_STATIC_IP/NET_STATIC_GW）。须在 ARP/回环自检前调用。 */
void e1000_dhcp_run(void);
uint32_t e1000_my_ip(void);   /* 本机 IP（DHCP 学得或静态兜底） */
uint32_t e1000_gw_ip(void);   /* 网关 IP（DHCP 学得或静态兜底） */

/* v0.28 DHCP 租期续约（v0.38 R1.3 后半：状态机移至用户态 dhcpclient）。
 * 内核只保留以下"原子能力"（机制），由用户态经 syscall#40-44 调用；
 * RFC 2131 §4.4.5 的续约/超时/重新获取决策在用户态进程（dhcpclient）。 */
void e1000_dhcp_query(uint32_t *lease_secs, uint32_t *elapsed,
                      uint32_t *t1_ticks, uint32_t *t2_ticks);  /* 查询租约 */
int  e1000_dhcp_send(uint32_t type, uint32_t req_ip);           /* 发一帧 */
int  e1000_dhcp_recv(uint32_t *mt, uint32_t *yi, uint32_t *si,  /* 收一条应答 */
                     uint32_t *rt, uint32_t *ls);
void e1000_dhcp_apply(uint32_t yi, uint32_t rt, uint32_t ls,    /* 应用 ACK */
                      uint32_t tag);
void e1000_dhcp_fallback(void);                                 /* 回退静态 */

#endif
