/* mini-os/v2-c-kernel/tests/test_ip.c
 * 宿主单元测试：极简 IPv4 头构建/解析 + 校验和。
 * 基准值 0x22C1 由独立 python 参考实现算得（避免"用被测函数自证"）。 */
#include "ip.h"
#include "utest.h"

int main(void) {
    /* ---- 校验和：全 0 的 20B 头 -> ~0 = 0xFFFF ---- */
    uint8_t z[20] = {0};
    CHECK_EQ(ip_checksum(z, 20), 0xFFFFu);
    /* 单字节（奇数长）：补 0 后求和取反 */
    uint8_t one[1] = {0x12};
    CHECK_EQ(ip_checksum(one, 1), 0xEDFFu);   /* sum=0x1200, ~0x1200 */

    /* ---- ip_build：已知参数的基准包 ----
     * src=0x0A00020F dst=0x0A000202 proto=17 plen=8 -> total=28, hdr_csum=0x22C1 */
    uint8_t pkt[64];
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t tot = ip_build(pkt, 0x0A00020Fu, 0x0A000202u, NET_PROTO_UDP, payload, 8);
    CHECK_EQ(tot, 28u);
    CHECK_EQ(pkt[0], 0x45u);                       /* 版本 4 + IHL 5 */
    CHECK_EQ(pkt[2], 0x00u); CHECK_EQ(pkt[3], 0x1Cu);   /* total = 28 */
    CHECK_EQ(pkt[8], 64u);                         /* TTL */
    CHECK_EQ(pkt[9], NET_PROTO_UDP);               /* proto */
    CHECK_EQ((pkt[10] << 8) | pkt[11], 0x22C1u);   /* 基准校验和 */
    CHECK_EQ(ip_checksum(pkt, 20), 0u);            /* 对完整头重算 = 0（round-trip） */
    CHECK_EQ(pkt[12], 0x0Au); CHECK_EQ(pkt[15], 0x0Fu);   /* src ip 10.0.2.15 */
    CHECK_EQ(pkt[16], 0x0Au); CHECK_EQ(pkt[19], 0x02u);   /* dst ip 10.0.2.2 */
    CHECK(pkt[20] == payload[0] && pkt[27] == payload[7]); /* 载荷就位 */

    /* ---- ip_parse：正确解析 ---- */
    uint32_t sip = 0; uint8_t proto = 0;
    const uint8_t *pay = 0; uint32_t plen = 0;
    CHECK_EQ(ip_parse(pkt, tot, &sip, &proto, &pay, &plen), 0);
    CHECK_EQ(sip, 0x0A00020Fu);
    CHECK_EQ(proto, NET_PROTO_UDP);
    CHECK_EQ(plen, 8u);
    CHECK(pay == pkt + 20);

    /* ---- 拒绝路径 ---- */
    CHECK(ip_parse(pkt, 19, &sip, &proto, &pay, &plen) < 0);      /* 太短 */
    uint8_t bad[40];
    for (int i = 0; i < 40; i++) bad[i] = pkt[i];
    bad[0] = 0x46;                                                  /* 版本错 */
    CHECK(ip_parse(bad, 40, &sip, &proto, &pay, &plen) < 0);
    bad[0] = 0x45; bad[10] ^= 0x01;                                /* 校验和损坏 */
    CHECK(ip_parse(bad, 40, &sip, &proto, &pay, &plen) < 0);
    bad[10] ^= 0x01; bad[3] = 0x80;                                /* total 超 len */
    CHECK(ip_parse(bad, 40, &sip, &proto, &pay, &plen) < 0);
    bad[3] = 0x28;                                                 /* 复原：tot=40 */

    /* ---- F5：分片一律拒（本栈无重组）+ 计数可观测 ----
     * 旧实现在此**放行**：非首片会带着自定 tot 通过并被 udp_parse 当作完整数据报交付。 */
    uint32_t base = ip_frag_dropped();
    uint8_t frg[40];
    for (int i = 0; i < 40; i++) frg[i] = pkt[i];
    frg[6] = 0x00; frg[7] = 0x01;                     /* 片偏移 = 1（非首片） */
    frg[10] = 0; frg[11] = 0; frg[10] = (uint8_t)(ip_checksum(frg, 20) >> 8);
    frg[11] = (uint8_t)(ip_checksum(frg, 20) & 0xFF); /* 重算校验和，排除"因校验和被拒" */
    CHECK(ip_parse(frg, 40, &sip, &proto, &pay, &plen) < 0);
    CHECK(frg[7] == 0x01);                            /* 确认这次拒的原因是分片段，非其他 */
    CHECK(ip_frag_dropped() == base + 1);             /* 计数 +1 */

    frg[6] = 0x20; frg[7] = 0x00;                     /* MF=1 首片：同样无路可走，必须拒 */
    frg[10] = 0; frg[11] = 0;
    frg[10] = (uint8_t)(ip_checksum(frg, 20) >> 8);
    frg[11] = (uint8_t)(ip_checksum(frg, 20) & 0xFF);
    CHECK(ip_parse(frg, 40, &sip, &proto, &pay, &plen) < 0);
    CHECK(ip_frag_dropped() == base + 2);

    frg[6] = 0x40; frg[7] = 0x00;                     /* DF=1 且偏移 0：正常数据报，必须放过 */
    frg[10] = 0; frg[11] = 0;                         /* —— 反向哨兵：防把"拒分片"写成"拒一切带 flags 的包" */
    frg[10] = (uint8_t)(ip_checksum(frg, 20) >> 8);
    frg[11] = (uint8_t)(ip_checksum(frg, 20) & 0xFF);
    CHECK(ip_parse(frg, 40, &sip, &proto, &pay, &plen) == 0);
    CHECK(ip_frag_dropped() == base + 2);             /* 计数不再增长 */

    UTEST_SUMMARY("test_ip");
}
