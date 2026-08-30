/* mini-os/v2-c-kernel/tests/test_icmp.c
 * ICMP Echo 宿主单元测试：只编译 src/net/icmp.c / ip.c / udp.c（纯逻辑），
 * 验证 Echo 请求构建-解析回读一致、校验和有效、错误输入能拒绝。
 */
#include "utest.h"
#include "icmp.h"
#include "ip.h"
#include "udp.h"

int main(void) {
    uint8_t frame[160];
    const uint8_t smac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const uint8_t dmac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x02 };
    uint32_t src = 0x0A00020Fu;    /* 10.0.2.15 */
    uint32_t dst = 0x0A000202u;    /* 10.0.2.2  */
    uint32_t src2, plen;
    uint8_t type, code;
    uint16_t id, seq;
    const uint8_t *pay;
    const uint8_t *pl = (const uint8_t *)"PING";

    /* 1) 构建 Echo 请求 -> 解析回读：以太网类型、IP 协议、type/id/seq/载荷、IP+ICMP 校验和均有效 */
    uint32_t flen = icmp_build_frame(frame, dmac, smac, src, dst, 0x4242u, 1, pl, 4);
    CHECK_EQ(flen, 14 + 20 + 8 + 4);
    CHECK_EQ(frame[12], 0x08); CHECK_EQ(frame[13], 0x00);        /* Ethernet II / IPv4 */
    CHECK_EQ(frame[23], NET_PROTO_ICMP);                          /* IP 协议=ICMP */
    CHECK_EQ(ip_checksum(frame + 14, 20), 0);                     /* IP 头校验和有效 */
    CHECK_EQ(icmp_parse(frame, flen, &src2, &type, &code, &id, &seq, &pay, &plen), 0);
    CHECK_EQ(src2, src);
    CHECK_EQ(type, ICMP_TYPE_ECHO_REQ);
    CHECK_EQ(code, 0);
    CHECK_EQ(id, 0x4242u);
    CHECK_EQ(seq, 1);
    CHECK_EQ(plen, 4);
    CHECK(pay[0] == 'P' && pay[1] == 'I' && pay[2] == 'N' && pay[3] == 'G');

    /* 2) 空载荷：总长 42，解析成功且 plen=0 */
    flen = icmp_build_frame(frame, dmac, smac, src, dst, 7, 2, 0, 0);
    CHECK_EQ(flen, 42);
    CHECK_EQ(icmp_parse(frame, flen, 0, &type, 0, &id, &seq, 0, &plen), 0);
    CHECK_EQ(type, ICMP_TYPE_ECHO_REQ);
    CHECK_EQ(id, 7);
    CHECK_EQ(seq, 2);
    CHECK_EQ(plen, 0);

    /* 3) 篡改载荷后校验和失效 -> 解析拒绝 */
    flen = icmp_build_frame(frame, dmac, smac, src, dst, 0x4242u, 1, pl, 4);
    frame[45] ^= 0xFF;                                             /* 载荷首字节翻转 */
    CHECK_EQ(icmp_parse(frame, flen, 0, 0, 0, 0, 0, 0, 0), -1);

    /* 4) UDP 帧给 ICMP 解析 -> 协议不匹配拒绝 */
    flen = udp_build_frame(frame, dmac, smac, src, dst, 7777, 7777, pl, 4);
    CHECK_EQ(icmp_parse(frame, flen, 0, 0, 0, 0, 0, 0, 0), -1);

    /* 5) 帧太短 -> 拒绝 */
    CHECK_EQ(icmp_parse(frame, 30, 0, 0, 0, 0, 0, 0, 0), -1);
    /* 5b) 短于以太网头（<14）-> len-14 下溢 + frame+14 越界守卫（fuzz 抓到的 BUG-029） */
    CHECK_EQ(icmp_parse(frame, 13, 0, 0, 0, 0, 0, 0, 0), -1);
    CHECK_EQ(icmp_parse(frame, 0, 0, 0, 0, 0, 0, 0, 0), -1);

    UTEST_SUMMARY("test_icmp");
}
