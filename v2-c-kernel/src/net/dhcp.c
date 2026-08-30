/* mini-os/v2-c-kernel/src/net/dhcp.c
 * 极简 DHCP 客户端（v0.25）：纯逻辑，不依赖内核/硬件。
 * BOOTP 固定头 236B：op|htype|hlen|hops|xid(4)|secs(2)|flags(2)|ciaddr(4)|yiaddr(4)|
 *   siaddr(4)|giaddr(4)|chaddr(16)|sname(64)|file(128) | cookie(4) | options...
 * 帧由 udp_build_frame 组装（0.0.0.0:68 -> 255.255.255.255:67，广播 MAC）。 */
#include "dhcp.h"
#include "udp.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* 构建 BOOTP 报文（固定头 + 选项），返回总长。
 * ciaddr：已租 IP（续约 RENEW/REBIND 用，初选为 0）；
 * with_server_id：REQUEST 是否带 server id(54)——SELECTING/RENEW 带，REBIND 不带。 */
static uint32_t build_bootp(uint8_t *bootp, const uint8_t *mac, uint32_t xid,
                            uint8_t msg_type, uint32_t server_ip, uint32_t req_ip,
                            uint32_t ciaddr, int with_server_id) {
    for (int i = 0; i < 240; i++) bootp[i] = 0;   /* 固定头 + cookie 清零 */
    bootp[0] = 1;                 /* op = BOOTREQUEST */
    bootp[1] = 1;                 /* htype = Ethernet */
    bootp[2] = 6;                 /* hlen = MAC 6B */
    put32(bootp + 4, xid);
    put16(bootp + 10, 0x8000u);   /* flags：请求广播应答（我们是轮询收包，广播更稳） */
    put32(bootp + 12, ciaddr);    /* ciaddr：续约时 = 已租 IP */
    for (int i = 0; i < 6; i++) bootp[28 + i] = mac[i];   /* chaddr */
    put32(bootp + 236, DHCP_MAGIC_COOKIE);

    uint32_t off = 240;
    bootp[off++] = 53; bootp[off++] = 1; bootp[off++] = msg_type;
    if (msg_type == DHCP_MSG_REQUEST) {
        if (with_server_id) {
            bootp[off++] = 54; bootp[off++] = 4; put32(bootp + off, server_ip); off += 4;
        }
        bootp[off++] = 50; bootp[off++] = 4; put32(bootp + off, req_ip);    off += 4;
    } else {
        /* 参数请求列表：1 子网掩码 / 3 路由器 / 51 租期 */
        bootp[off++] = 55; bootp[off++] = 3;
        bootp[off++] = 1; bootp[off++] = 3; bootp[off++] = 51;
    }
    bootp[off++] = 255;           /* end option */
    return off;
}

uint32_t dhcp_build_discover(uint8_t *frame, const uint8_t *mac, uint32_t xid) {
    uint8_t bootp[300];
    uint32_t blen = build_bootp(bootp, mac, xid, DHCP_MSG_DISCOVER, 0, 0, 0, 0);
    const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return udp_build_frame(frame, bcast_mac, mac,
                           0x00000000u, 0xFFFFFFFFu,          /* 0.0.0.0 -> 255.255.255.255 */
                           DHCP_CLIENT_PORT, DHCP_SERVER_PORT, bootp, blen);
}

uint32_t dhcp_build_request(uint8_t *frame, const uint8_t *mac, uint32_t xid,
                            uint32_t server_ip, uint32_t req_ip) {
    uint8_t bootp[300];
    uint32_t blen = build_bootp(bootp, mac, xid, DHCP_MSG_REQUEST, server_ip, req_ip, 0, 1);
    const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return udp_build_frame(frame, bcast_mac, mac,
                           0x00000000u, 0xFFFFFFFFu,
                           DHCP_CLIENT_PORT, DHCP_SERVER_PORT, bootp, blen);
}

/* v0.28 RENEW：单播 REQUEST 到服务器（src=ciaddr:68 -> server_ip:67），带 54+50 */
uint32_t dhcp_build_renew(uint8_t *frame, const uint8_t *mac, const uint8_t *dst_mac,
                          uint32_t xid, uint32_t ciaddr, uint32_t server_ip) {
    uint8_t bootp[300];
    uint32_t blen = build_bootp(bootp, mac, xid, DHCP_MSG_REQUEST, server_ip, ciaddr,
                                ciaddr, 1);
    return udp_build_frame(frame, dst_mac, mac, ciaddr, server_ip,
                           DHCP_CLIENT_PORT, DHCP_SERVER_PORT, bootp, blen);
}

/* v0.28 REBIND：广播 REQUEST（src=ciaddr:68 -> 255.255.255.255:67），只带 50 */
uint32_t dhcp_build_rebind(uint8_t *frame, const uint8_t *mac, uint32_t xid,
                           uint32_t ciaddr) {
    uint8_t bootp[300];
    const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t blen = build_bootp(bootp, mac, xid, DHCP_MSG_REQUEST, 0, ciaddr, ciaddr, 0);
    return udp_build_frame(frame, bcast_mac, mac, ciaddr, 0xFFFFFFFFu,
                           DHCP_CLIENT_PORT, DHCP_SERVER_PORT, bootp, blen);
}

int dhcp_parse_reply(const uint8_t *payload, uint32_t plen, uint32_t xid,
                     uint8_t *msg_type, uint32_t *yiaddr,
                     uint32_t *server_ip, uint32_t *router, uint32_t *lease) {
    if (plen < 240) return -1;                         /* 固定头 236 + cookie 4 */
    if (payload[0] != 2) return -1;                    /* op = BOOTREPLY */
    if (get32(payload + 4) != xid) return -1;          /* 事务号不匹配 */
    if (get32(payload + 236) != DHCP_MAGIC_COOKIE) return -1;

    uint8_t type = 0;
    uint32_t srv = 0, rtr = 0, le = 0;
    uint32_t off = 240;
    while (off < plen) {
        uint8_t code = payload[off++];
        if (code == 255) break;                        /* end option */
        if (code == 0) continue;                       /* pad */
        if (off + 1 > plen) return -1;
        uint8_t olen = payload[off++];
        if (off + olen > plen) return -1;
        if      (code == 53 && olen >= 1) type = payload[off];
        else if (code == 54 && olen >= 4) srv = get32(payload + off);
        else if (code ==  3 && olen >= 4) rtr = get32(payload + off);  /* router(网关) */
        else if (code == 51 && olen >= 4) le = get32(payload + off);   /* 租期 */
        off += olen;
    }
    if (type == 0) return -1;                          /* 必须有消息类型 */

    if (msg_type)  *msg_type = type;
    if (yiaddr)    *yiaddr = get32(payload + 16);      /* 分配给客户的 IP */
    if (server_ip) *server_ip = srv;
    if (router)    *router = rtr;
    if (lease)     *lease = le;
    return 0;
}
