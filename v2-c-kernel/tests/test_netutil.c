/* mini-os/v2-c-kernel/tests/test_netutil.c
 * v0.18 以太网/ARP 帧构建与解析宿主单元测试（纯逻辑）。
 * 编译：gcc -Isrc src/netutil.c tests/test_netutil.c -o tests/build/test_netutil */
#include "netutil.h"
#include "utest.h"
#include <stdint.h>

int main(void) {
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

    /* ---- 构建 ARP 请求 ---- */
    uint8_t f[64];
    int len = net_build_arp_request(f, mac, 0x0A00020Fu, 0x0A000202u);   /* 10.0.2.15 -> 10.0.2.2 */
    CHECK(len == 42);
    for (int i = 0; i < 6; i++) CHECK(f[i] == 0xFF);            /* 以太网 dst = 广播 */
    for (int i = 0; i < 6; i++) CHECK(f[6 + i] == mac[i]);      /* 以太网 src */
    CHECK(f[12] == 0x08 && f[13] == 0x06);                      /* type = ARP */
    CHECK(f[14] == 0 && f[15] == 1);                            /* htype = Ethernet */
    CHECK(f[16] == 0x08 && f[17] == 0x00);                      /* ptype = IPv4 */
    CHECK(f[18] == 6 && f[19] == 4);                            /* hlen/plen */
    CHECK(f[20] == 0 && f[21] == 1);                            /* op = request */
    for (int i = 0; i < 6; i++) CHECK(f[22 + i] == mac[i]);     /* sha */
    CHECK(f[28] == 10 && f[29] == 0 && f[30] == 2 && f[31] == 15);   /* spa = 10.0.2.15 */
    for (int i = 0; i < 6; i++) CHECK(f[32 + i] == 0);          /* tha = 0 */
    CHECK(f[38] == 10 && f[39] == 0 && f[40] == 2 && f[41] == 2);    /* tpa = 10.0.2.2 */

    /* ---- 以太网类型 ---- */
    uint16_t t = 0;
    CHECK(net_eth_type(f, 42, &t) == 0 && t == NET_ETH_TYPE_ARP);
    CHECK(net_eth_type(f, 10, &t) == -1);                       /* 帧太短 */

    /* ---- 解析 ARP 应答（手工构造） ---- */
    uint8_t rm[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x99};
    uint8_t reply[64];
    for (int i = 0; i < 6; i++) { reply[i] = mac[i]; reply[6 + i] = rm[i]; }
    reply[12] = 0x08; reply[13] = 0x06;
    reply[14] = 0; reply[15] = 1;
    reply[16] = 0x08; reply[17] = 0x00;
    reply[18] = 6; reply[19] = 4;
    reply[20] = 0; reply[21] = 2;                               /* op = reply */
    for (int i = 0; i < 6; i++) reply[22 + i] = rm[i];          /* sha */
    reply[28] = 10; reply[29] = 0; reply[30] = 2; reply[31] = 2;/* spa = 10.0.2.2 */
    for (int i = 0; i < 6; i++) reply[32 + i] = mac[i];         /* tha */
    reply[38] = 10; reply[39] = 0; reply[40] = 2; reply[41] = 15;

    uint32_t sip = 0;
    uint8_t sm[6] = {0};
    CHECK(net_parse_arp_reply(reply, 42, &sip, sm) == 0);
    CHECK(sip == 0x0A000202u);
    for (int i = 0; i < 6; i++) CHECK(sm[i] == rm[i]);

    /* ---- 错误输入 ---- */
    CHECK(net_parse_arp_reply(reply, 30, &sip, sm) == -1);      /* 太短 */
    reply[21] = 1;                                              /* op=request 非应答 */
    CHECK(net_parse_arp_reply(reply, 42, &sip, sm) == -1);

    UTEST_SUMMARY("test_netutil");
}
