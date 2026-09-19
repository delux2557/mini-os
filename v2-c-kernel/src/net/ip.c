/* mini-os/v2-c-kernel/src/net/ip.c
 * 极简 IPv4（v0.19）：纯逻辑（大端网络字节序），不依赖内核/硬件。
 * 头 20B：ver/ihl | tos | tot_len | id | flags/frag | ttl | proto | hdr_csum | src | dst */
#include "ip.h"

uint16_t ip_checksum(const uint8_t *buf, uint32_t len) {
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
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

uint32_t ip_build(uint8_t *frame, uint32_t src_ip, uint32_t dst_ip,
                  uint8_t proto, const uint8_t *payload, uint32_t plen) {
    uint32_t tot = 20u + plen;
    frame[0] = 0x45;                     /* 版本 4 + IHL 5 */
    frame[1] = 0;                        /* TOS */
    put16(frame + 2, (uint16_t)tot);     /* 总长 */
    put16(frame + 4, 0);                 /* ID */
    put16(frame + 6, 0x4000u);           /* DF（不分片） */
    frame[8] = 64;                       /* TTL */
    frame[9] = proto;
    put16(frame + 10, 0);                /* 校验和先置 0 */
    put32(frame + 12, src_ip);
    put32(frame + 16, dst_ip);
    for (uint32_t i = 0; i < plen; i++) frame[20 + i] = payload[i];
    put16(frame + 10, ip_checksum(frame, 20));
    return tot;
}

/* 分片丢弃计数：协议层不打日志（与 icmp/udp/netutil 同风格，日志归 netsock 分发处），
 * 但"拒了多少"必须可观测——静默丢弃本身就是一种失真（安全复核反复撞到的同一形态）。 */
static uint32_t s_frag_dropped;

uint32_t ip_frag_dropped(void) { return s_frag_dropped; }

int ip_parse(const uint8_t *frame, uint32_t len, uint32_t *src_ip,
             uint8_t *proto, const uint8_t **payload, uint32_t *plen) {
    if (len < 20) return -1;
    if (frame[0] != 0x45u) return -1;                    /* 只支持 IPv4 + 20B 头 */
    uint32_t tot = ((uint32_t)frame[2] << 8) | frame[3];
    if (tot < 20 || tot > len) return -1;
    /* 安全复核 F5：本栈不做分片重组 ⇒ 分片必须在此显式拒绝，不能"看不见"。
     * 旧实现从不读 flags/fragment offset（frame[6..7]），于是**非首片**（offset≠0）
     * 会带着攻击者自定的 tot 长度通过 ip_parse，再被 udp_parse 当作**完整 UDP 数据报**
     * 交付给 socket —— 载荷内容与长度全由外来方指定（实测：offset=1/5 + 校验和=0 即成功）。
     * 首片（MF=1）同样拒：我们没有任何重组路径，收下它等于收下注定残缺的数据报。
     * 合法性依据（静态取证，故拒绝是零功能回归）：本栈所有入向合法路径都不会分片——
     * syscall 侧 iov.max≤1400、netsock 侧 plen≤NET_RXMAX、单帧 NET_ETH_FRAME_MAX=1518、
     * SLIP 侧 SLIP_MAX=1600，且 ip_build 自发自带 DF（frame[6..7]=0x4000）。 */
    uint16_t fl = ((uint16_t)frame[6] << 8) | frame[7];
    if ((fl & 0x1FFFu) != 0 || (fl & 0x2000u) != 0) {    /* 片偏移≠0 或 MF=1 */
        s_frag_dropped++;
        return -1;
    }
    if (ip_checksum(frame, 20) != 0) return -1;          /* 校验和有效时重算得 0 */
    if (src_ip) *src_ip = ((uint32_t)frame[12] << 24) | ((uint32_t)frame[13] << 16) |
                          ((uint32_t)frame[14] << 8) | frame[15];
    if (proto)  *proto = frame[9];
    if (payload) *payload = frame + 20;
    if (plen)    *plen = tot - 20;
    return 0;
}
