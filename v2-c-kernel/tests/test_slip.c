/* mini-os/v2-c-kernel/tests/test_slip.c
 * 宿主单元测试：SLIP（RFC 1055）封包/解包（路线图 v1.1 Step 2）。
 * 覆盖：成帧 + 转义（数据中出现 0xC0/0xDB）、增量解码、分隔 END、协议错误、溢出、round-trip。
 */
#include "slip.h"
#include "utest.h"
#include <string.h>

int main(void) {
    /* ---- 编码：帧 = [END | 转义 | END] ----
     * 数据含特殊字节：0xC0(END)->ESC_END 0xDC，0xDB(ESC)->ESC_ESC 0xDD；0xDC/0xDD 非特殊，原样透传 */
    const uint8_t pkt[] = {0xAB, 0xC0, 0xDB, 0x01, 0xDC, 0x00, 0xDD};
    const uint8_t want[] = {SLIP_END, 0xAB, SLIP_ESC, SLIP_ESC_END,
                            SLIP_ESC, SLIP_ESC_ESC, 0x01, 0xDC, 0x00, 0xDD, SLIP_END};
    uint8_t wire[SLIP_ENC_BOUND(sizeof pkt)];
    uint32_t wl = slip_frame_encode(pkt, sizeof pkt, wire);
    CHECK_EQ(wl, sizeof want);
    CHECK(memcmp(wire, want, sizeof want) == 0);

    /* ---- 解码：逐字节喂入（模拟串口字节流），END 得完整一帧 ---- */
    slip_rx_t rx;
    slip_rx_init(&rx);
    uint8_t got[64];
    uint32_t gl = 0;
    int got_frame = 0;
    for (uint32_t i = 0; i < wl; i++) {
        int st = slip_rx_feed(&rx, wire[i]);
        if (st == 1) { memcpy(got, rx.buf, rx.len); gl = rx.len; got_frame = 1; break; }
        CHECK_EQ(st, 0);
    }
    CHECK(got_frame);
    CHECK_EQ(gl, sizeof pkt);
    CHECK(memcmp(got, pkt, sizeof pkt) == 0);      /* round-trip 还原含 0xC0/0xDB 的数据 */

    /* 收到帧后复位，可继续收下一帧（连续两帧） */
    slip_rx_reset(&rx);
    uint8_t pkt2[3] = {0x11, 0x22, 0x33};
    uint8_t wire2[32];
    uint32_t wl2 = slip_frame_encode(pkt2, 3, wire2);
    got_frame = 0;
    for (uint32_t i = 0; i < wl2; i++) {
        int st = slip_rx_feed(&rx, wire2[i]);
        if (st == 1) { memcpy(got, rx.buf, rx.len); gl = rx.len; got_frame = 1; break; }
    }
    CHECK(got_frame);
    CHECK_EQ(gl, 3u);
    CHECK(memcmp(got, pkt2, 3) == 0);
    slip_rx_reset(&rx);

    /* ---- 边界：分隔/前导 END 不产出帧（len=0 的 END 是分隔符） ---- */
    CHECK_EQ(slip_rx_feed(&rx, SLIP_END), 0);      /* 前导 END，无帧 */
    CHECK_EQ(slip_rx_feed(&rx, 0x41), 0);
    CHECK_EQ(slip_rx_feed(&rx, SLIP_END), 1);      /* 收尾 END，得 1 字节帧 */
    CHECK_EQ(rx.len, 1u); CHECK_EQ(rx.buf[0], 0x41u);
    slip_rx_reset(&rx);

    /* ---- 协议错误：ESC 后接非法转义体 -> -1，帧作废 ---- */
    slip_rx_init(&rx);
    CHECK_EQ(slip_rx_feed(&rx, SLIP_ESC), 0);
    CHECK_EQ(slip_rx_feed(&rx, 0x41), -1);         /* DB 后必须 DC/DD */

    /* ---- 溢出：非特殊字节超过 SLIP_MAX -> -1 ---- */
    {
        slip_rx_t ro;
        slip_rx_init(&ro);
        int st = 0;
        for (int i = 0; i < SLIP_MAX; i++) { st = slip_rx_feed(&ro, 0x7F); if (st) break; }
        CHECK_EQ(st, 0);
        CHECK_EQ(slip_rx_feed(&ro, 0x7F), -1);     /* 第 SLIP_MAX+1 非特殊字节越界 */
    }

    /* ---- 空载荷帧：encode 出 [END,END]（都被当作分隔符，不产出帧） ---- */
    {
        uint8_t e[4];
        uint32_t el = slip_frame_encode((const uint8_t *)"", 0, e);
        CHECK_EQ(el, 2u);
        CHECK_EQ(e[0], SLIP_END); CHECK_EQ(e[1], SLIP_END);
    }

    UTEST_SUMMARY("test_slip");
}