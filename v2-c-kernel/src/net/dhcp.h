/* mini-os/v2-c-kernel/src/net/dhcp.h
 * 极简 DHCP 客户端（v0.25）：BOOTP/DHCP 报文构建/解析（纯逻辑，可宿主单测）。
 * 协议：RFC 2131/2132 —— DISCOVER/OFFER/REQUEST/ACK over UDP 67(server)/68(client)。
 * 上层驱动先 dhcp_build_* 发请求，收到 UDP 载荷后交给 dhcp_parse_reply。 */
#ifndef NET_DHCP_H
#define NET_DHCP_H

#include <stdint.h>

#define DHCP_SERVER_PORT 67u
#define DHCP_CLIENT_PORT 68u
#define DHCP_MAGIC_COOKIE 0x63825363u   /* 99.130.83.99 */

/* DHCP 消息类型（option 53） */
#define DHCP_MSG_DISCOVER 1u
#define DHCP_MSG_OFFER    2u
#define DHCP_MSG_REQUEST  3u
#define DHCP_MSG_ACK      5u
#define DHCP_MSG_NAK      6u

/* 静态 IP 兜底（"静态 IP 可配置化"的单一配置点：DHCP 失败时回退的地址） */
#define NET_STATIC_IP 0x0A00020Fu   /* 10.0.2.15：SLIRP 分配给客户机的地址 */
#define NET_STATIC_GW 0x0A000202u   /* 10.0.2.2 ：SLIRP 网关/DNS 别名 */

/* 构建完整以太网帧（Ethernet+IPv4+UDP+BOOTP），返回帧总长。
 * DISCOVER：src 0.0.0.0:68 -> 广播 255.255.255.255:67，带参数请求列表(1/3/51)；
 * REQUEST ：携带 server id(54) + 请求 IP(50)。 */
uint32_t dhcp_build_discover(uint8_t *frame, const uint8_t *mac, uint32_t xid);
uint32_t dhcp_build_request(uint8_t *frame, const uint8_t *mac, uint32_t xid,
                            uint32_t server_ip, uint32_t req_ip);

/* 解析 DHCP 应答载荷（BOOTP 头 + 选项）：xid 不匹配 / 缺少 magic cookie / 无消息类型 -> -1。
 * 成功置 msg_type（OFFER/ACK/...）、yiaddr（分配 IP）、server_ip(54)、router(3)、lease(51)。 */
int dhcp_parse_reply(const uint8_t *payload, uint32_t plen, uint32_t xid,
                     uint8_t *msg_type, uint32_t *yiaddr,
                     uint32_t *server_ip, uint32_t *router, uint32_t *lease);

#endif /* NET_DHCP_H */
