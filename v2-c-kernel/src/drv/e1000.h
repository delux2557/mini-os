/* mini-os/v2-c-kernel/src/e1000.h
 * Intel 82540EM (e1000) 驱动（v0.18）：QEMU 默认网卡，MMIO + 描述符环，纯轮询。
 */
#ifndef _E1000_H
#define _E1000_H
#include <stdint.h>

int e1000_init(void);                 /* 探测 + 初始化；成功返回 0 */
int e1000_ready(void);                /* 驱动是否就绪 */
const uint8_t *e1000_mac(void);       /* 6 字节 MAC */

/* v0.20: 网关（10.0.2.2）MAC——ARP 自检学到；供上层 socket 发送寻址用；未学到返回 NULL */
const uint8_t *e1000_gw_mac(void);

/* 发一帧（数据自 data 拷入内部 TX 缓冲，等待设备取走后返回）；成功 0 */
int e1000_tx(const uint8_t *data, uint32_t len);

/* 收一帧：无包返回 0，收到则拷入 buf 并置 *len 返回 1，失败 -1 */
int e1000_rx(uint8_t *buf, uint32_t max, uint32_t *len);

/* 启动自检：发 ARP 请求（who has 10.0.2.2）并等待 SLIRP 回复，打印里程碑；
 * 学到网关 MAC 后缓存，供 e1000_udp_selftest 使用 */
void e1000_selftest(void);

/* v0.19 UDP 回环自检：向宿主 127.0.0.1:7777 的 echo 服务发 PING 收 PONG */
void e1000_udp_selftest(void);

/* v0.23 ICMP Echo 自检：发 Echo 请求到 SLIRP 网关 10.0.2.2，收其 Echo 应答（PING 通宿主） */
void e1000_icmp_selftest(void);

/* v0.25 DHCP 客户端：DISCOVER->OFFER->REQUEST->ACK 从 SLIRP DHCP 服务器动态获取
 * IP/网关；失败回退静态地址（NET_STATIC_IP/NET_STATIC_GW）。须在 ARP/回环自检前调用。 */
void e1000_dhcp_run(void);
uint32_t e1000_my_ip(void);   /* 本机 IP（DHCP 学得或静态兜底） */
uint32_t e1000_gw_ip(void);   /* 网关 IP（DHCP 学得或静态兜底） */

/* v0.28 DHCP 租期续约：由 timer 心跳每 tick 驱动（非阻塞，每 tick 至多收一帧）。
 * 到 T1(0.5×lease) 单播 RENEW、到 T2(0.875×lease) 广播 REBIND，ACK 重置定时器；
 * 编译期 DHCP_RENEW_SECS 非空时覆盖服务器租期（测试用短租期）。 */
void e1000_dhcp_tick(void);

#endif
