/* mini-os/v2-c-kernel/src/netutil.h
 * 极简以太网/ARP 帧构建与解析（v0.18）：纯逻辑，不依赖内核/硬件，可宿主单测。
 * 为 v0.19 的 ARP/IP/ICMP/UDP 协议栈铺路。
 */
#ifndef _NETUTIL_H
#define _NETUTIL_H
#include <stdint.h>

#define NET_ETH_TYPE_ARP 0x0806u
#define NET_ETH_TYPE_IP  0x0800u

/* 构建"以太网 II + ARP 请求"帧到 frame（须 >= 60 字节）；返回帧长（42） */
int net_build_arp_request(uint8_t *frame, const uint8_t *src_mac,
                          uint32_t src_ip, uint32_t target_ip);

/* 取以太网类型（II 帧第 12-13 字节）；帧太短返回 -1 */
int net_eth_type(const uint8_t *frame, uint32_t len, uint16_t *etype);

/* 解析 ARP 应答：成功返回 0，填 sender_ip（网络序转主机序）与 sender_mac */
int net_parse_arp_reply(const uint8_t *frame, uint32_t len,
                        uint32_t *sender_ip, uint8_t *sender_mac);

#endif
