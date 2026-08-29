/* mini-os/v2-c-kernel/src/net/icmp.h
 * 极简 ICMP over IPv4（v0.23）：Echo 请求/应答（PING）。
 * 纯逻辑，不依赖内核/硬件，可宿主单测。
 * 帧布局：Ethernet(14) | IPv4(20) | ICMP(8) | payload
 * ICMP Echo 头：type(1) code(1) checksum(2) id(2) seq(2)，校验和只覆盖 ICMP 报文（RFC 792）。 */
#ifndef NET_ICMP_H
#define NET_ICMP_H

#include <stdint.h>
#include "ip.h"

#define ICMP_TYPE_ECHO_REQ 8u
#define ICMP_TYPE_ECHO_REP 0u

/* 构建完整以太网帧（Ethernet+IPv4+ICMP Echo 请求），返回帧总长（42+plen）。
 * dst_mac/src_mac 填以太网头；src_ip/dst_ip 填 IP 头。调用方保证 frame >= 42+plen。 */
uint32_t icmp_build_frame(uint8_t *frame,
                          const uint8_t *dst_mac, const uint8_t *src_mac,
                          uint32_t src_ip, uint32_t dst_ip,
                          uint16_t id, uint16_t seq,
                          const uint8_t *payload, uint32_t plen);

/* 解析以太网+IPv4+ICMP：成功返回 0，填源 IP、type/code、id/seq、载荷指针（指向 frame 内）与长度。
 * 校验 ICMP 校验和（重算为 0）；非 ICMP 协议或校验失败返回 -1。 */
int icmp_parse(const uint8_t *frame, uint32_t len, uint32_t *src_ip,
               uint8_t *type, uint8_t *code,
               uint16_t *id, uint16_t *seq,
               const uint8_t **payload, uint32_t *plen);

#endif /* NET_ICMP_H */
