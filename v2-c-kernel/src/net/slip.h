/* mini-os/v2-c-kernel/src/net/slip.h
 * SLIP（RFC 1055）封包/解包（路线图 v1.1 Step 2，D1：IP 数据报 over 串口）。
 * 纯逻辑、无内核/硬件依赖 => 可宿主单测。
 *   特殊字节：END 0xC0 = 帧分隔；ESC 0xDB；ESC_END 0xDC（遥 0xC0）；ESC_ESC 0xDD（遥 0xDB）。
 * 包单位 = 上层 IP 数据报：发送方对数据报做 [END | 转义字节 | END] 成帧写串口；
 * 接收方增量解码，遇到非空的 END 即得一帧完整 IP 数据报。
 */
#ifndef NET_SLIP_H
#define NET_SLIP_H

#include <stdint.h>

#define SLIP_END    0xC0u
#define SLIP_ESC    0xDBu
#define SLIP_ESC_END 0xDCu
#define SLIP_ESC_ESC 0xDDu

#define SLIP_MAX     1600   /* 单帧（IP 数据报）上限，与 netif 收包缓冲一致 */

/* 编码回界：最坏全转义 + 前后各一 END */
#define SLIP_ENC_BOUND(len) (2u * (len) + 2u)

/* 把一帧 IP 数据报编码为 SLIP 线上字节流（[END|转义|END]）到 out；
 * 调用方保证 out >= SLIP_ENC_BOUND(len)；返回线上字节数。 */
uint32_t slip_frame_encode(const uint8_t *pkt, uint32_t len, uint8_t *out);

/* 增量解码器状态 */
typedef struct {
    uint8_t  buf[SLIP_MAX];
    uint32_t len;
    int      escaped;      /* 上一字节为 ESC，下一字节是转义体 */
} slip_rx_t;

void    slip_rx_init(slip_rx_t *r);
void    slip_rx_reset(slip_rx_t *r);   /* 丢弃当前半成品（复位到空帧） */
/* 喂入一个串口字节：
 *   1 = 收到一个完整 IP 数据报（在 r->buf/r->len，调用方拷出后应 slip_rx_reset）
 *   0 = 尚未凑齐一帧（分隔 END 或仍累积中）
 *  -1 = SLIP 协议/溢出错误（无线长度 END / 越界），帧作废 */
int     slip_rx_feed(slip_rx_t *r, uint8_t b);

#endif /* NET_SLIP_H */