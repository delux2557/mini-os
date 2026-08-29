/* mini-os/v2-c-kernel/src/net/udp.c
 * 极简 UDP over IPv4（v0.19）：纯逻辑，不依赖内核/硬件。
 * 校验和：伪头(12B: srcIP|dstIP|0|17|ulen) + UDP 头 + 载荷；载荷就地构建无需拷贝缓冲。 */
#include "udp.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* 增量求和（RFC 1071 累加，不折叠） */
static uint32_t chksum_add(uint32_t sum, const uint8_t *buf, uint32_t len) {
    while (len > 1) {
        sum += (uint16_t)((uint16_t)(buf[0] << 8) | buf[1]);
        buf += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)(buf[0] << 8);
    return sum;
}
static uint16_t chksum_fin(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* 校验接收帧的 UDP 校验和（RFC 768）：伪头(12B: srcIP|dstIP|0|17|ulen) + UDP 头 + 载荷。
 * 校验和字段为 0 -> 发送端未计算（IPv4 允许），视为有效；
 * 否则重算（含校验和字段的完整数据报）须折叠为 0 才有效。 */
static int udp_checksum_valid(uint32_t src_ip, uint32_t dst_ip,
                              const uint8_t *udp, uint32_t ulen) {
    if (get16(udp + 6) == 0) return 0;                 /* 未计算校验和，跳过验证 */
    uint8_t pseudo[12];
    put32(pseudo, src_ip);  put32(pseudo + 4, dst_ip);
    pseudo[8] = 0;  pseudo[9] = NET_PROTO_UDP;
    put16(pseudo + 10, (uint16_t)ulen);
    uint32_t sum = chksum_add(0, pseudo, 12);
    sum = chksum_add(sum, udp, ulen);                  /* UDP 头(8B) + 载荷 */
    return chksum_fin(sum) == 0 ? 0 : -1;
}

uint32_t udp_build_frame(uint8_t *frame, const uint8_t *dst_mac, const uint8_t *src_mac,
                         uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         const uint8_t *payload, uint32_t plen) {
    /* Ethernet 头 @0 */
    for (int i = 0; i < 6; i++) { frame[i] = dst_mac[i]; frame[6 + i] = src_mac[i]; }
    put16(frame + 12, NET_ETH_TYPE_IPV4);

    /* 载荷 @42，UDP 头 @34（校验和先 0） */
    for (uint32_t i = 0; i < plen; i++) frame[42 + i] = payload[i];
    put16(frame + 34, src_port);
    put16(frame + 36, dst_port);
    put16(frame + 38, (uint16_t)(8u + plen));
    put16(frame + 40, 0);

    /* UDP 校验和：伪头(12B) + UDP 头(8B) + 载荷 */
    uint8_t pseudo[12];
    put32(pseudo, src_ip);  put32(pseudo + 4, dst_ip);
    pseudo[8] = 0;  pseudo[9] = NET_PROTO_UDP;
    put16(pseudo + 10, (uint16_t)(8u + plen));
    uint32_t sum = chksum_add(0, pseudo, 12);
    sum = chksum_add(sum, frame + 34, 8);
    sum = chksum_add(sum, frame + 42, plen);
    uint16_t cs = chksum_fin(sum);
    if (cs == 0) cs = 0xFFFFu;             /* RFC 768：算得 0 必须以全 1 发送（0 表示"未计算"） */
    put16(frame + 40, cs);

    /* IP 头 @14（最后填，含自身校验和） */
    frame[14] = 0x45;
    frame[15] = 0;
    put16(frame + 16, (uint16_t)(20u + 8u + plen));
    put16(frame + 18, 0);
    put16(frame + 20, 0x4000u);
    frame[22] = 64;
    frame[23] = NET_PROTO_UDP;
    put16(frame + 24, 0);
    put32(frame + 26, src_ip);
    put32(frame + 30, dst_ip);
    put16(frame + 24, ip_checksum(frame + 14, 20));

    return 42u + plen;
}

int udp_parse(const uint8_t *frame, uint32_t len, uint32_t *src_ip,
              uint16_t *src_port, uint16_t *dst_port,
              const uint8_t **payload, uint32_t *plen) {
    if (len < 14) return -1;
    if (get16(frame + 12) != NET_ETH_TYPE_IPV4) return -1;

    const uint8_t *udp = 0;
    uint32_t udp_len = 0;
    uint8_t proto = 0;
    uint32_t sip = 0;
    if (ip_parse(frame + 14, len - 14, &sip, &proto, &udp, &udp_len) < 0) return -1;
    if (proto != NET_PROTO_UDP) return -1;
    if (udp_len < 8) return -1;

    uint32_t ulen = get16(udp + 4);
    if (ulen < 8 || ulen > udp_len) return -1;

    /* v0.24 校验和错误路径：伪头用 IP 头里的 src/dst（dst 从 frame[30..33] 读，
     * IP 头校验和已由 ip_parse 验证），损坏帧一律拒绝（netsock 分发据此丢包） */
    uint32_t dip = ((uint32_t)frame[30] << 24) | ((uint32_t)frame[31] << 16) |
                   ((uint32_t)frame[32] << 8) | frame[33];
    if (udp_checksum_valid(sip, dip, udp, ulen) != 0) return -1;

    if (src_ip) *src_ip = sip;
    if (src_port) *src_port = get16(udp);
    if (dst_port) *dst_port = get16(udp + 2);
    if (payload) *payload = udp + 8;
    if (plen)    *plen = ulen - 8;
    return 0;
}
