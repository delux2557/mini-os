/* mini-os/v2-c-kernel/tests/test_udp.c
 * 宿主单元测试：完整以太网+IPv4+UDP 帧构建与解析。
 * 基准值（IP 0x22BC / UDP 0xE0FE / 47B）由独立 python 参考实现算得。
 * 注意：UDP 伪头协议字节是 0x11（17），不是 0x17。 */
#include "udp.h"
#include "utest.h"

int main(void) {
    const uint8_t src_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    const uint8_t dst_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    const uint32_t src_ip = 0x0A00020Fu;   /* 10.0.2.15 */
    const uint32_t dst_ip = 0x0A000202u;   /* 10.0.2.2  */
    const uint8_t payload[] = "HELLO";     /* 5B */
    uint8_t frame[128];
    const uint8_t *pay = 0;

    /* ---- 构建 ---- */
    uint32_t flen = udp_build_frame(frame, dst_mac, src_mac, src_ip, dst_ip,
                                    1234, 7777, payload, 5);
    CHECK_EQ(flen, 47u);                       /* 14 + 20 + 8 + 5 */

    /* 以太网头 */
    CHECK_EQ(frame[12], 0x08u); CHECK_EQ(frame[13], 0x00u);   /* IPv4 */
    int mac_ok = 1;
    for (int i = 0; i < 6; i++) if (frame[i] != dst_mac[i]) mac_ok = 0;
    for (int i = 0; i < 6; i++) if (frame[6 + i] != src_mac[i]) mac_ok = 0;
    CHECK(mac_ok);

    /* IP 头 @14 */
    CHECK_EQ((frame[16] << 8) | frame[17], 33u);       /* total = 20+8+5 */
    CHECK_EQ(frame[23], NET_PROTO_UDP);
    CHECK_EQ((frame[24] << 8) | frame[25], 0x22BCu);   /* 基准 IP 校验和 */
    CHECK_EQ(ip_checksum(frame + 14, 20), 0u);

    /* UDP 头 @34 */
    CHECK_EQ((frame[34] << 8) | frame[35], 1234u);     /* src port */
    CHECK_EQ((frame[36] << 8) | frame[37], 7777u);     /* dst port */
    CHECK_EQ((frame[38] << 8) | frame[39], 13u);       /* udp len = 8+5 */
    CHECK_EQ((frame[40] << 8) | frame[41], 0xE0FEu);   /* 基准 UDP 校验和 */
    int pay_ok = 1;
    for (int i = 0; i < 5; i++) if (frame[42 + i] != payload[i]) pay_ok = 0;
    CHECK(pay_ok);

    /* ---- 解析 round-trip ---- */
    uint32_t sip = 0; uint16_t sp = 0, dp = 0; uint32_t plen = 0;
    CHECK_EQ(udp_parse(frame, flen, &sip, &sp, &dp, &pay, &plen), 0);
    CHECK_EQ(sip, src_ip);
    CHECK_EQ(sp, 1234u);
    CHECK_EQ(dp, 7777u);
    CHECK_EQ(plen, 5u);
    CHECK(pay == frame + 42);
    CHECK(pay[0] == 'H' && pay[4] == 'O');

    /* ---- 拒绝路径 ---- */
    CHECK(udp_parse(frame, 13, &sip, &sp, &dp, &pay, &plen) < 0);   /* 不足以太网头 */
    uint8_t notip[128];
    for (int i = 0; i < 47; i++) notip[i] = frame[i];
    notip[13] = 0x06;                                               /* 非 IPv4 类型 */
    CHECK(udp_parse(notip, 47, &sip, &sp, &dp, &pay, &plen) < 0);
    for (int i = 0; i < 47; i++) notip[i] = frame[i];
    notip[23] = 1;                                                  /* IP 载荷非 UDP */
    CHECK(udp_parse(notip, 47, &sip, &sp, &dp, &pay, &plen) < 0);
    for (int i = 0; i < 47; i++) notip[i] = frame[i];
    notip[24] ^= 1;                                                 /* IP 校验和损坏 */
    CHECK(udp_parse(notip, 47, &sip, &sp, &dp, &pay, &plen) < 0);

    /* ---- v0.24 校验和错误路径 ---- */
    /* 载荷被篡改 -> 校验和不符，拒绝 */
    for (int i = 0; i < 47; i++) notip[i] = frame[i];
    notip[42] ^= 0xFF;                                              /* 载荷首字节翻转 */
    CHECK(udp_parse(notip, 47, &sip, &sp, &dp, &pay, &plen) < 0);
    /* UDP 校验和字段被篡改 -> 拒绝 */
    for (int i = 0; i < 47; i++) notip[i] = frame[i];
    notip[40] ^= 0x80;                                              /* 校验和字段翻转 */
    CHECK(udp_parse(notip, 47, &sip, &sp, &dp, &pay, &plen) < 0);
    /* 源 IP 被改（并修复 IP 头校验和）-> 伪头与校验时不符，拒绝（证明伪头参与校验） */
    for (int i = 0; i < 47; i++) notip[i] = frame[i];
    notip[26] ^= 0x01;                                              /* src IP 首字节翻转 */
    notip[24] = 0; notip[25] = 0;                                   /* 重算 IP 头校验和 */
    { uint16_t cs = ip_checksum(notip + 14, 20); notip[24] = (uint8_t)(cs >> 8); notip[25] = (uint8_t)cs; }
    CHECK(udp_parse(notip, 47, &sip, &sp, &dp, &pay, &plen) < 0);
    /* 校验和字段为 0 -> 发送端未计算（RFC 768 允许），接受 */
    for (int i = 0; i < 47; i++) notip[i] = frame[i];
    notip[40] = 0; notip[41] = 0;
    CHECK(udp_parse(notip, 47, &sip, &sp, &dp, &pay, &plen) == 0);
    CHECK_EQ(sp, 1234u); CHECK_EQ(plen, 5u);

    /* ---- v1.1 Step 1：UDP/IP 数据报级构建/解析（netif 包单位，无链路层头）---- */
    /* 构建 IP 数据报（IPv4@0, UDP@20, 载荷@28） */
    uint8_t ip[128];
    uint32_t iplen = udp_build_ip(ip, src_ip, dst_ip, 1234, 7777, payload, 5);
    CHECK_EQ(iplen, 33u);                        /* 20 + 8 + 5 */
    CHECK_EQ((ip[0] >> 4), 4u);                  /* 版本 = IPv4 */
    CHECK_EQ((ip[2] << 8) | ip[3], 33u);         /* IP 总长 */
    CHECK_EQ(ip[9], NET_PROTO_UDP);
    CHECK_EQ(ip_checksum(ip, 20), 0u);           /* IP 头校验和有效 */
    CHECK_EQ((ip[20] << 8) | ip[21], 1234u);     /* UDP src port */
    CHECK_EQ((ip[22] << 8) | ip[23], 7777u);     /* UDP dst port */
    CHECK_EQ((ip[24] << 8) | ip[25], 13u);       /* UDP 长度 = 8+5 */
    /* 与 eth 帧内嵌的 IP 数据报逐字节一致（udp_build_frame 复用 udp_build_ip） */
    int same = 1;
    for (uint32_t i = 0; i < iplen; i++) if (ip[i] != frame[14 + i]) same = 0;
    CHECK(same);
    /* 数据报级解析 round-trip */
    CHECK_EQ(udp_parse_ip(ip, iplen, &sip, &sp, &dp, &pay, &plen), 0);
    CHECK_EQ(sip, src_ip);
    CHECK_EQ(sp, 1234u);
    CHECK_EQ(dp, 7777u);
    CHECK_EQ(plen, 5u);
    CHECK(pay == ip + 28);
    /* 拒绝路径：载荷篡改 -> UDP 校验和拒绝 */
    for (uint32_t i = 0; i < iplen; i++) notip[i] = ip[i];
    notip[28] ^= 0xFF;
    CHECK(udp_parse_ip(notip, iplen, &sip, &sp, &dp, &pay, &plen) < 0);
    /* 拒绝路径：过短 / 非 UDP 协议 */
    CHECK(udp_parse_ip(ip, 13, &sip, &sp, &dp, &pay, &plen) < 0);
    for (uint32_t i = 0; i < iplen; i++) notip[i] = ip[i];
    notip[9] = 1;                                /* proto=ICMP */
    CHECK(udp_parse_ip(notip, iplen, &sip, &sp, &dp, &pay, &plen) < 0);

    UTEST_SUMMARY("test_udp");
}
