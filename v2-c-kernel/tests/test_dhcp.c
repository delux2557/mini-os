/* mini-os/v2-c-kernel/tests/test_dhcp.c
 * 宿主单元测试：DHCP 报文构建（DISCOVER/REQUEST）与应答解析（OFFER/ACK）。
 * 覆盖：帧结构/字段、UDP round-trip、xid 匹配、magic cookie、选项解析、拒绝路径。 */
#include "dhcp.h"
#include "udp.h"
#include "utest.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* 手工构造 DHCP 应答载荷（BOOTP 头 + cookie + 选项） */
static uint32_t build_reply(uint8_t *buf, uint32_t xid, uint8_t msg_type,
                            uint32_t yiaddr, uint32_t server_ip, uint32_t lease,
                            int with_cookie, int with_type) {
    memset(buf, 0, 260);
    buf[0] = 2;                                   /* BOOTREPLY */
    buf[1] = 1; buf[2] = 6;
    put32(buf + 4, xid);
    put32(buf + 16, yiaddr);
    if (with_cookie) put32(buf + 236, DHCP_MAGIC_COOKIE);
    uint32_t off = 240;
    if (with_type) { buf[off++] = 53; buf[off++] = 1; buf[off++] = msg_type; }
    buf[off++] = 54; buf[off++] = 4; put32(buf + off, server_ip); off += 4;
    buf[off++] =  3; buf[off++] = 4; put32(buf + off, 0x0A000202u); off += 4; /* router */
    buf[off++] = 51; buf[off++] = 4; put32(buf + off, lease);     off += 4;
    buf[off++] = 255;
    return off;
}

int main(void) {
    const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    const uint32_t xid = 0x7F6D1234u;
    uint8_t frame[320];

    /* ---- DISCOVER 构建 ---- */
    uint32_t flen = dhcp_build_discover(frame, mac, xid);
    CHECK_EQ(flen, 42u + 249u);                   /* 14+20+8 + bootp(240+3+5+1) */
    CHECK_EQ(frame[12], 0x08u); CHECK_EQ(frame[13], 0x00u);   /* IPv4 */
    int bcast_ok = 1;
    for (int i = 0; i < 6; i++) if (frame[i] != 0xFFu) bcast_ok = 0;      /* dst=广播 */
    for (int i = 0; i < 6; i++) if (frame[6 + i] != mac[i]) bcast_ok = 0; /* src=MAC */
    CHECK(bcast_ok);
    CHECK_EQ(frame[23], NET_PROTO_UDP);
    int zero_src = 1, bcast_dst = 1;
    for (int i = 0; i < 4; i++) if (frame[26 + i] != 0) zero_src = 0;     /* src IP 0.0.0.0 */
    for (int i = 0; i < 4; i++) if (frame[30 + i] != 0xFFu) bcast_dst = 0; /* dst 255.255.255.255 */
    CHECK(zero_src && bcast_dst);
    CHECK_EQ((frame[34] << 8) | frame[35], DHCP_CLIENT_PORT);   /* 68 */
    CHECK_EQ((frame[36] << 8) | frame[37], DHCP_SERVER_PORT);   /* 67 */
    CHECK_EQ(ip_checksum(frame + 14, 20), 0u);
    /* BOOTP 头 @42 */
    CHECK_EQ(frame[42], 1u);                       /* op=BOOTREQUEST */
    CHECK_EQ(frame[43], 1u);                       /* htype */
    CHECK_EQ(frame[44], 6u);                       /* hlen */
    CHECK_EQ((frame[46] << 24) | (frame[47] << 16) | (frame[48] << 8) | frame[49], xid);
    int mac_ok = 1;
    for (int i = 0; i < 6; i++) if (frame[42 + 28 + i] != mac[i]) mac_ok = 0;
    CHECK(mac_ok);
    CHECK_EQ((frame[42 + 236] << 24) | (frame[42 + 237] << 16) | (frame[42 + 238] << 8) | frame[42 + 239],
             DHCP_MAGIC_COOKIE);
    CHECK_EQ(frame[42 + 240], 53u); CHECK_EQ(frame[42 + 242], DHCP_MSG_DISCOVER);  /* option53=1 */
    /* UDP round-trip */
    uint32_t sip = 0, plen = 0; uint16_t sp = 0, dp = 0; const uint8_t *pay = 0;
    CHECK_EQ(udp_parse(frame, flen, &sip, &sp, &dp, &pay, &plen), 0);
    CHECK_EQ(sp, DHCP_CLIENT_PORT); CHECK_EQ(dp, DHCP_SERVER_PORT);
    CHECK_EQ(plen, 249u);

    /* ---- REQUEST 构建：带 server id(54) + 请求 IP(50) ---- */
    uint32_t rlen = dhcp_build_request(frame, mac, xid, 0x0A000202u, 0x0A00020Fu);
    CHECK_EQ(rlen, 42u + 256u);                    /* bootp(240+3+6+6+1) */
    CHECK_EQ(frame[42], 1u);
    int srv_ok = 0, req_ok = 0;
    for (uint32_t i = 42 + 240; i + 1 < rlen; i++) {
        if (frame[i] == 54 && frame[i + 1] == 4 &&
            (frame[i + 2] << 24 | frame[i + 3] << 16 | frame[i + 4] << 8 | frame[i + 5]) == 0x0A000202u)
            srv_ok = 1;
        if (frame[i] == 50 && frame[i + 1] == 4 &&
            (frame[i + 2] << 24 | frame[i + 3] << 16 | frame[i + 4] << 8 | frame[i + 5]) == 0x0A00020Fu)
            req_ok = 1;
    }
    CHECK(srv_ok && req_ok);

    /* ---- 应答解析 ---- */
    uint8_t rep[280];
    uint32_t blen, yi, si, rt, ls; uint8_t mt;
    blen = build_reply(rep, xid, DHCP_MSG_OFFER, 0x0A00020Fu, 0x0A000202u, 3600, 1, 1);
    CHECK_EQ(dhcp_parse_reply(rep, blen, xid, &mt, &yi, &si, &rt, &ls), 0);
    CHECK_EQ(mt, DHCP_MSG_OFFER); CHECK_EQ(yi, 0x0A00020Fu);
    CHECK_EQ(si, 0x0A000202u); CHECK_EQ(rt, 0x0A000202u); CHECK_EQ(ls, 3600u);
    blen = build_reply(rep, xid, DHCP_MSG_ACK, 0x0A00020Fu, 0x0A000202u, 1800, 1, 1);
    CHECK_EQ(dhcp_parse_reply(rep, blen, xid, &mt, &yi, &si, &rt, &ls), 0);
    CHECK_EQ(mt, DHCP_MSG_ACK); CHECK_EQ(ls, 1800u);

    /* ---- 拒绝路径 ---- */
    blen = build_reply(rep, xid + 1, DHCP_MSG_OFFER, 0x0A00020Fu, 0x0A000202u, 3600, 1, 1);
    CHECK(dhcp_parse_reply(rep, blen, xid, 0, 0, 0, 0, 0) < 0);    /* xid 不匹配 */
    blen = build_reply(rep, xid, DHCP_MSG_OFFER, 0x0A00020Fu, 0x0A000202u, 3600, 0, 1);
    CHECK(dhcp_parse_reply(rep, blen, xid, 0, 0, 0, 0, 0) < 0);    /* 缺 magic cookie */
    blen = build_reply(rep, xid, DHCP_MSG_OFFER, 0x0A00020Fu, 0x0A000202u, 3600, 1, 0);
    CHECK(dhcp_parse_reply(rep, blen, xid, 0, 0, 0, 0, 0) < 0);    /* 无消息类型 */
    CHECK(dhcp_parse_reply(rep, 239, xid, 0, 0, 0, 0, 0) < 0);     /* 过短 */
    memset(rep, 0, 240); rep[0] = 1; put32(rep + 4, xid);
    put32(rep + 236, DHCP_MAGIC_COOKIE);
    CHECK(dhcp_parse_reply(rep, 240, xid, 0, 0, 0, 0, 0) < 0);     /* op=1 非应答 */

    UTEST_SUMMARY("test_dhcp");
}
