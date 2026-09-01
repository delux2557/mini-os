/* mini-os/v2-c-kernel/src/net/slip.c
 * SLIP（RFC 1055）封包/解包纯逻辑实现（路线图 v1.1 Step 2，D1）。
 */
#include "slip.h"

uint32_t slip_frame_encode(const uint8_t *pkt, uint32_t len, uint8_t *out) {
    uint32_t o = 0;
    out[o++] = SLIP_END;                       /* 帧起始 END */
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = pkt[i];
        if (b == SLIP_END)      { out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_END; }
        else if (b == SLIP_ESC) { out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_ESC; }
        else                    { out[o++] = b; }
    }
    out[o++] = SLIP_END;                       /* 帧结束 END */
    return o;
}

void slip_rx_init(slip_rx_t *r) {
    r->len = 0;
    r->escaped = 0;
}

void slip_rx_reset(slip_rx_t *r) {
    r->len = 0;
    r->escaped = 0;
}

int slip_rx_feed(slip_rx_t *r, uint8_t b) {
    if (r->escaped) {
        r->escaped = 0;
        uint8_t val;
        if (b == SLIP_ESC_END)      val = SLIP_END;
        else if (b == SLIP_ESC_ESC) val = SLIP_ESC;
        else return -1;                        /* ESC 后跟非法转义体 -> 协议错误 */
        if (r->len >= SLIP_MAX) return -1;     /* 溢出 */
        r->buf[r->len++] = val;
        return 0;
    }
    if (b == SLIP_END) {
        if (r->len == 0) return 0;             /* 分隔 / 前导 END，忽略 */
        return 1;                              /* 完整一帧就绪 */
    }
    if (b == SLIP_ESC) { r->escaped = 1; return 0; }
    if (r->len >= SLIP_MAX) return -1;         /* 溢出 */
    r->buf[r->len++] = b;
    return 0;
}