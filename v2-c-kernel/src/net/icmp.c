/* mini-os/v2-c-kernel/src/net/icmp.c
 * 极简 ICMP over IPv4（v0.23）：Echo 请求构建 + 应答解析（PING）。
 * 纯逻辑（大端网络字节序），不依赖内核/硬件，可宿主单测。
 * ICMP 校验和只覆盖 ICMP 报文本身（type/code/csum/id/seq/载荷），无伪头（RFC 792）。 */
#include "icmp.h"
#include "ip.h"

static uint16_t icmp_checksum(const uint8_t *buf, uint32_t len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)((uint16_t)(buf[0] << 8) | buf[1]);
        buf += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)(buf[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

uint32_t icmp_build_frame(uint8_t *frame, const uint8_t *dst_mac, const uint8_t *src_mac,
                          uint32_t src_ip, uint32_t dst_ip,
                          uint16_t id, uint16_t seq,
                          const uint8_t *payload, uint32_t plen) {
    /* Ethernet II */
    for (int i = 0; i < 6; i++) { frame[i] = dst_mac[i]; frame[6 + i] = src_mac[i]; }
    frame[12] = 0x08; frame[13] = 0x00;
    /* IPv4（20B）：proto=ICMP，总长为 IP 头 + ICMP */
    uint32_t iplen = 20u + 8u + plen;
    frame[14] = 0x45;
    frame[15] = 0;
    put16(frame + 16, (uint16_t)iplen);
    put16(frame + 18, 0);
    put16(frame + 20, 0x4000u);                 /* DF */
    frame[22] = 64;                             /* TTL */
    frame[23] = NET_PROTO_ICMP;
    put16(frame + 24, 0);                       /* 校验和先置 0 */
    frame[26] = (uint8_t)(src_ip >> 24); frame[27] = (uint8_t)(src_ip >> 16);
    frame[28] = (uint8_t)(src_ip >> 8);  frame[29] = (uint8_t)src_ip;
    frame[30] = (uint8_t)(dst_ip >> 24); frame[31] = (uint8_t)(dst_ip >> 16);
    frame[32] = (uint8_t)(dst_ip >> 8);  frame[33] = (uint8_t)dst_ip;
    /* ICMP Echo 请求（8B 头 + 载荷） */
    frame[34] = ICMP_TYPE_ECHO_REQ;
    frame[35] = 0;
    put16(frame + 36, 0);                       /* 校验和先置 0 */
    put16(frame + 38, id);
    put16(frame + 40, seq);
    for (uint32_t i = 0; i < plen; i++) frame[42 + i] = payload[i];
    /* 先算 IP 头校验和，再算 ICMP 校验和 */
    put16(frame + 24, ip_checksum(frame + 14, 20));
    put16(frame + 36, icmp_checksum(frame + 34, 8 + plen));
    return 14u + iplen;
}

int icmp_parse(const uint8_t *frame, uint32_t len, uint32_t *src_ip,
               uint8_t *type, uint8_t *code,
               uint16_t *id, uint16_t *seq,
               const uint8_t **payload, uint32_t *plen) {
    if (len < 14) return -1;                 /* 短于以太网头：防止 len-14 下溢 + frame+14 越界（fuzz 抓到） */
    uint32_t sip = 0;
    uint8_t proto = 0;
    const uint8_t *icmp = 0;
    uint32_t icmp_len = 0;
    if (ip_parse(frame + 14, len - 14, &sip, &proto, &icmp, &icmp_len) < 0) return -1;
    if (proto != NET_PROTO_ICMP) return -1;
    if (icmp_len < 8) return -1;
    if (icmp_checksum(icmp, icmp_len) != 0) return -1;   /* 校验和有效时重算得 0 */
    if (src_ip) *src_ip = sip;
    if (type)   *type = icmp[0];
    if (code)   *code = icmp[1];
    if (id)     *id = (uint16_t)((uint16_t)(icmp[4] << 8) | icmp[5]);
    if (seq)    *seq = (uint16_t)((uint16_t)(icmp[6] << 8) | icmp[7]);
    if (payload) *payload = icmp + 8;
    if (plen)    *plen = icmp_len - 8;
    return 0;
}
