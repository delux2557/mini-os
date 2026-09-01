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

/* 构建 **UDP/IP 数据报**（IPv4 @0，UDP @20，载荷 @28）——netif 抽象层的包单位（D1）。
 * 链路层封装（以太网头/SLIP）由网卡适配层负责：e1000 适配器把此数据报加以太网头。
 * 返回数据报总长（20+8+plen）。 */
uint32_t udp_build_ip(uint8_t *ip, uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      const uint8_t *payload, uint32_t plen) {
    /* 载荷 @28，UDP 头 @20（校验和先 0） */
    for (uint32_t i = 0; i < plen; i++) ip[28 + i] = payload[i];
    put16(ip + 20, src_port);
    put16(ip + 22, dst_port);
    put16(ip + 24, (uint16_t)(8u + plen));
    put16(ip + 26, 0);

    /* UDP 校验和：伪头(12B) + UDP 头(8B) + 载荷 */
    uint8_t pseudo[12];
    put32(pseudo, src_ip);  put32(pseudo + 4, dst_ip);
    pseudo[8] = 0;  pseudo[9] = NET_PROTO_UDP;
    put16(pseudo + 10, (uint16_t)(8u + plen));
    uint32_t sum = chksum_add(0, pseudo, 12);
    sum = chksum_add(sum, ip + 20, 8);
    sum = chksum_add(sum, ip + 28, plen);
    uint16_t cs = chksum_fin(sum);
    if (cs == 0) cs = 0xFFFFu;             /* RFC 768：算得 0 必须以全 1 发送（0 表示"未计算"） */
    put16(ip + 26, cs);

    /* IP 头 @0（最后填，含自身校验和） */
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)(20u + 8u + plen));
    put16(ip + 4, 0);
    put16(ip + 6, 0x4000u);
    ip[8] = 64;
    ip[9] = NET_PROTO_UDP;
    put16(ip + 10, 0);
    put32(ip + 12, src_ip);
    put32(ip + 16, dst_ip);
    put16(ip + 10, ip_checksum(ip, 20));

    return 28u + plen;
}

/* 构建完整以太网帧（Ethernet+IPv4+UDP）——透传给宿主测试与旧调用方保留；
 * eth 头由 udp_build_frame 负责（测试参考），净道生产路径（netsock）已改走
 * udp_build_ip + netif（eth 头下沉到 e1000 适配层）。返回 14+28+plen。 */
uint32_t udp_build_frame(uint8_t *frame, const uint8_t *dst_mac, const uint8_t *src_mac,
                         uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         const uint8_t *payload, uint32_t plen) {
    /* Ethernet 头 @0 */
    for (int i = 0; i < 6; i++) { frame[i] = dst_mac[i]; frame[6 + i] = src_mac[i]; }
    put16(frame + 12, NET_ETH_TYPE_IPV4);

    /* IP 数据报 @14 */
    uint32_t iplen = udp_build_ip(frame + 14, src_ip, dst_ip,
                                  src_port, dst_port, payload, plen);
    return 14u + iplen;
}

/* UDP/IP 数据报解析核心（IP 头 @0）：校验 IP 头 + UDP 校验和，输出源/目的端口与载荷。 */
static int udp_parse_ip_core(const uint8_t *ip, uint32_t len, uint32_t *src_ip,
                             uint16_t *src_port, uint16_t *dst_port,
                             const uint8_t **payload, uint32_t *plen) {
    const uint8_t *udp = 0;
    uint32_t udp_len = 0;
    uint8_t proto = 0;
    uint32_t sip = 0;
    if (ip_parse(ip, len, &sip, &proto, &udp, &udp_len) < 0) return -1;
    if (proto != NET_PROTO_UDP) return -1;
    if (udp_len < 8) return -1;

    uint32_t ulen = get16(udp + 4);
    if (ulen < 8 || ulen > udp_len) return -1;

    /* v0.24 校验和错误路径：伪头用 IP 头里的 src/dst（dst 取 IP 头 [16..19]，
     * IP 头校验和已由 ip_parse 验证），损坏帧一律拒绝（netsock 分发据此丢包） */
    uint32_t dip = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) |
                   ((uint32_t)ip[18] << 8) | ip[19];
    if (udp_checksum_valid(sip, dip, udp, ulen) != 0) return -1;

    if (src_ip) *src_ip = sip;
    if (src_port) *src_port = get16(udp);
    if (dst_port) *dst_port = get16(udp + 2);
    if (payload) *payload = udp + 8;
    if (plen)    *plen = ulen - 8;
    return 0;
}

/* 解析 UDP/IP 数据报（IP 头 @0）——netif 收包路径用（适配层已剥链路层头）。 */
int udp_parse_ip(const uint8_t *ip, uint32_t len, uint32_t *src_ip,
                 uint16_t *src_port, uint16_t *dst_port,
                 const uint8_t **payload, uint32_t *plen) {
    return udp_parse_ip_core(ip, len, src_ip, src_port, dst_port, payload, plen);
}

/* 解析以太网+IPv4+UDP（含链路层头）——宿主测试与旧调用方保留。 */
int udp_parse(const uint8_t *frame, uint32_t len, uint32_t *src_ip,
              uint16_t *src_port, uint16_t *dst_port,
              const uint8_t **payload, uint32_t *plen) {
    if (len < 14) return -1;
    if (get16(frame + 12) != NET_ETH_TYPE_IPV4) return -1;
    return udp_parse_ip_core(frame + 14, len - 14, src_ip, src_port, dst_port, payload, plen);
}
