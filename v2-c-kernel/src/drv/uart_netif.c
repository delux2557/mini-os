/* mini-os/v2-c-kernel/src/drv/uart_netif.c
 * COM2 串口网卡适配层（路线图 v1.1 Step 2）：SLIP（RFC 1055）封装，与 e1000 平级。
 *   - D1：包单位为 IP 数据报；SLIP 就是 IP over 串口的链路层封装（RFC 1055）。
 *   - COM2 = 0x2F8（COM1 0x3F8 被 shell 终端占用，严禁复用，D3）。
 *   - 轮询收发（无中断/DMA），与 e1000 的轮询风格一致；适配层只做 SLIP 成帧 + 字节搬运。
 *   - netif rx 契约：1=收到一个完整 IP 数据报 / 0=暂无完整帧（串口为字节流，需凑齐一帧）。
 * 本层是内核态（io 指令访串口寄存器），SLIP 编解码纯逻辑在 src/net/slip.c（可宿主单测）。
 */
#include "netif.h"
#include "slip.h"
#include "serial.h"      /* serial_printf/literal 调试输出走 COM1 */
#include <stdint.h>

#define COM2       0x2F8u
#define COM2_IER   (COM2 + 1u)
#define COM2_LCR   (COM2 + 3u)
#define COM2_LSR   (COM2 + 5u)
#define LSR_TX_RDY 0x20u   /* 发送保持寄存器空 */
#define LSR_RX_RDY 0x01u   /* 接收数据就绪 */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static slip_rx_t slip_rx;

static int uart_if_init(void) {
    /* 探测：写回读校验 LCR（无设备读回非写入值） */
    outb(COM2_LCR, 0x03);
    if (inb(COM2_LCR) != 0x03) return -1;
    outb(COM2_IER, 0x00);            /* 关中断（纯轮询） */
    outb(COM2_LCR, 0x80);            /* DLAB */
    outb(COM2, 0x03);                /* 38400 波特率低字节 */
    outb(COM2 + 1u, 0x00);           /* 高字节 */
    outb(COM2_LCR, 0x03);            /* 8N1 */
    slip_rx_init(&slip_rx);
    serial_printf("[uart_netif] COM2 SLIP up (0x2F8)\n");
    return 0;
}

static int uart_if_ready(void) { return 0; }

/* tx：把 IP 数据报做成 SLIP 帧（[END|转义|END]）逐字节写 COM2 */
static int uart_if_tx(const uint8_t *ip, uint32_t len) {
    uint8_t w[SLIP_ENC_BOUND(SLIP_MAX)];
    uint32_t n = slip_frame_encode(ip, len, w);
    for (uint32_t i = 0; i < n; i++) {
        while ((inb(COM2_LSR) & LSR_TX_RDY) == 0) { }
        outb(COM2, w[i]);
    }
    return 0;
}

/* rx：把当前可读字节喂 SLIP 解码器；凑齐一帧即给上层一个完整 IP 数据报。 */
static int uart_if_rx(uint8_t *buf, uint32_t max, uint32_t *outlen) {
    while (inb(COM2_LSR) & LSR_RX_RDY) {
        int st = slip_rx_feed(&slip_rx, inb(COM2));
        if (st > 0) {                 /* 完整一帧 */
            uint32_t n = slip_rx.len;
            if (n <= max) {
                for (uint32_t i = 0; i < n; i++) buf[i] = slip_rx.buf[i];
                *outlen = n;
            } else {
                *outlen = 0;          /* 超长帧丢弃 */
            }
            slip_rx_reset(&slip_rx);
            return 1;                 /* 本次交付一帧，其余字节留待下次排空 */
        } else if (st < 0) {
            slip_rx_reset(&slip_rx);  /* SLIP 协议错误：丢帧续收 */
        }
    }
    return 0;
}

static const uint8_t *uart_if_mac(void) { return 0; }   /* 串口无 MAC（D1 不做假 MAC） */

static const netif_ops_t uart_netif_ops = {
    uart_if_init, uart_if_ready, uart_if_tx, uart_if_rx, uart_if_mac
};

void uart_netif_register(void) { netif_register(&uart_netif_ops); }