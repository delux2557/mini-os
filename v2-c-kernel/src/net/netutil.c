/* mini-os/v2-c-kernel/src/netutil.c
 * 极简以太网/ARP 帧构建与解析（v0.18）。
 * 布局（全部大端，网络字节序）：
 *   以太网 II：dst[6] | src[6] | type[2]
 *   ARP：htype[2]=1 | ptype[2]=0x0800 | hlen=6 | plen=4 | op[2] | sha[6] | spa[4] | tha[6] | tpa[4]
 */
#include "netutil.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

int net_build_arp_request(uint8_t *f, const uint8_t *src_mac,
                          uint32_t src_ip, uint32_t target_ip) {
    for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6 + i] = src_mac[i]; }  /* 广播 -> src */
    put16(f + 12, NET_ETH_TYPE_ARP);
    put16(f + 14, 1);                    /* htype = Ethernet */
    put16(f + 16, 0x0800u);              /* ptype = IPv4 */
    f[18] = 6; f[19] = 4;                /* hlen / plen */
    put16(f + 20, 1);                    /* op = request */
    for (int i = 0; i < 6; i++) f[22 + i] = src_mac[i];   /* sha */
    put32(f + 28, src_ip);               /* spa */
    for (int i = 0; i < 6; i++) f[32 + i] = 0;            /* tha = 0 */
    put32(f + 38, target_ip);            /* tpa */
    return 42;
}

int net_eth_type(const uint8_t *f, uint32_t len, uint16_t *etype) {
    if (len < 14) return -1;
    *etype = (uint16_t)((f[12] << 8) | f[13]);
    return 0;
}

int net_parse_arp_reply(const uint8_t *f, uint32_t len,
                        uint32_t *sender_ip, uint8_t *sender_mac) {
    if (len < 42) return -1;
    if ((f[12] << 8 | f[13]) != NET_ETH_TYPE_ARP) return -1;
    if (!(f[20] == 0 && f[21] == 2)) return -1;            /* op = reply */
    for (int i = 0; i < 6; i++) sender_mac[i] = f[22 + i]; /* sha */
    *sender_ip = ((uint32_t)f[28] << 24) | ((uint32_t)f[29] << 16) |
                 ((uint32_t)f[30] << 8) | (uint32_t)f[31]; /* spa */
    return 0;
}
