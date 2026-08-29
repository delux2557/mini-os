/* mini-os/v2-c-kernel/serial.h */
#ifndef _SERIAL_H
#define _SERIAL_H
#include <stdint.h>

/* 串口接收回调：收到一个字符时调用（如 kb_feed_char，可为 0 关闭转发）。
 * 返回"是否入环形缓冲"（由 kb_feed_char 约定；调用方忽略）。 */
typedef int (*serial_rx_hook_t)(char);

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_printf(const char *fmt, ...);

/* ---- v0.10 接收通道（IRQ4） ---- */
int  serial_rx_ready(void);                 /* 接收缓冲是否有数据 */
char serial_getc(void);                     /* 读一个接收字符 */
void serial_set_rx_hook(serial_rx_hook_t fn); /* 注册接收回调 */

#endif
