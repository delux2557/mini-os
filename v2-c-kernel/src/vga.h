/* mini-os/v2-c-kernel/vga.h */
#ifndef _VGA_H
#define _VGA_H
#include <stdint.h>

void vga_init(void);
void vga_clear(uint8_t attr);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_printf(const char *fmt, ...);
/* 重写指定行的内容（用于状态栏等固定行），会清空整行 */
void vga_row(int row, uint8_t attr, const char *fmt, ...);

#endif
