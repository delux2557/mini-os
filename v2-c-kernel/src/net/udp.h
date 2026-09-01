/* mini-os/v2-c-kernel/src/net/udp.h
 * 极简 UDP over IPv4（v0.19）：完整以太网帧构建 + 解析（纯逻辑，可宿主单测）。
 * 帧布局：Ethernet(14) | IPv4(20) | UDP(8) | payload */
#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdint.h>
#include "ip.h"

/* 构建 UDP/IP 数据报（IPv4 @0，UDP @20，载荷 @28），返回数据报总长（28+plen）。
 * 链路层封装由网卡适配层负责（e1000 适配器加以太网头）；src_ip/dst_ip 用于 IP 头
 * 与 UDP 伪头校验和。调用方保证 ip >= 28+plen。 */
uint32_t udp_build_ip(uint8_t *ip,
                      uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      const uint8_t *payload, uint32_t plen);

/* 构建完整以太网帧（Ethernet+IPv4+UDP），返回帧总长（42+plen）。
 * dst_mac/src_mac 填以太网头；src_ip/dst_ip 用于 IP 头与 UDP 伪头校验和。
 * 校验和按 RFC 768 计算（伪头 + UDP 头 + 载荷），调用方保证 frame >= 42+plen。
 * 说明：socket 生产路径（netsock）走 udp_build_ip + netif（链路层封装由网卡适配层
 * 负责）。本函数为**共享 etherframe 参考**，消费方是 e1000 链路路径：dhcp.c 的
 * BOOTP 组帧（over e1000，HAL 期收口）与 e1000 UDP selftest，以及宿主测试的基准。 */
uint32_t udp_build_frame(uint8_t *frame,
                         const uint8_t *dst_mac, const uint8_t *src_mac,
                         uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         const uint8_t *payload, uint32_t plen);

/* 解析 UDP/IP 数据报（IP 头 @0）：返回源 IP、源/目的端口、载荷指针与长度。
 * 供 netif 收包路径使用（适配层已剥链路层头）。 */
int udp_parse_ip(const uint8_t *ip, uint32_t len, uint32_t *src_ip,
                 uint16_t *src_port, uint16_t *dst_port,
                 const uint8_t **payload, uint32_t *plen);

/* 解析以太网+IPv4+UDP：返回源 IP、源/目的端口、载荷指针（指向 frame 内）与长度。
 * 供宿主测试/旧调用方保留。 */
int udp_parse(const uint8_t *frame, uint32_t len, uint32_t *src_ip,
              uint16_t *src_port, uint16_t *dst_port,
              const uint8_t **payload, uint32_t *plen);

#endif /* NET_UDP_H */
