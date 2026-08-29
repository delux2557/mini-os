/* mini-os/v2-c-kernel/src/net/ip.h
 * 极简 IPv4（v0.19）：头部构建/解析 + 16 位校验和（纯逻辑，可宿主单测）。 */
#ifndef NET_IP_H
#define NET_IP_H

#include <stdint.h>

#define NET_ETH_TYPE_IPV4 0x0800u
#define NET_PROTO_ICMP    1u
#define NET_PROTO_UDP     17u

/* 16 位 ones-complement 校验和（RFC 1071，对"已含校验和字段"的完整头再算得 0） */
uint16_t ip_checksum(const uint8_t *buf, uint32_t len);

/* 构建 IPv4 头(20B)+载荷到 frame（调用方保证 frame >= 20+plen），返回 IP 包总长 */
uint32_t ip_build(uint8_t *frame, uint32_t src_ip, uint32_t dst_ip,
                  uint8_t proto, const uint8_t *payload, uint32_t plen);

/* 解析 IPv4 头：校验版本/IHL/总长/校验和，返回源 IP、协议、载荷指针（指向 frame 内）与长度 */
int ip_parse(const uint8_t *frame, uint32_t len, uint32_t *src_ip,
             uint8_t *proto, const uint8_t **payload, uint32_t *plen);

#endif /* NET_IP_H */
