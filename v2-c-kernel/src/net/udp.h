/* mini-os/v2-c-kernel/src/net/udp.h
 * 极简 UDP over IPv4（v0.19）：完整以太网帧构建 + 解析（纯逻辑，可宿主单测）。
 * 帧布局：Ethernet(14) | IPv4(20) | UDP(8) | payload */
#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdint.h>
#include "ip.h"

/* 构建完整以太网帧（Ethernet+IPv4+UDP），返回帧总长（42+plen）。
 * dst_mac/src_mac 填以太网头；src_ip/dst_ip 用于 IP 头与 UDP 伪头校验和。
 * 校验和按 RFC 768 计算（伪头 + UDP 头 + 载荷），调用方保证 frame >= 42+plen。 */
uint32_t udp_build_frame(uint8_t *frame,
                         const uint8_t *dst_mac, const uint8_t *src_mac,
                         uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         const uint8_t *payload, uint32_t plen);

/* 解析以太网+IPv4+UDP：返回源 IP、源/目的端口、载荷指针（指向 frame 内）与长度 */
int udp_parse(const uint8_t *frame, uint32_t len, uint32_t *src_ip,
              uint16_t *src_port, uint16_t *dst_port,
              const uint8_t **payload, uint32_t *plen);

#endif /* NET_UDP_H */
