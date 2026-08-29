/* mini-os/v2-c-kernel/vga.c
 * VGA 80x25 文本模式驱动：直接写显存 0xB8000，带光标与滚动 */
#include "vga.h"
#include <stdarg.h>

#define FB_ADDR 0xB8000
#define COLS 80
#define ROWS 25

static volatile uint16_t *const fb = (volatile uint16_t *)FB_ADDR;
static uint8_t cur_attr = 0x0F;
static int cur_row = 0, cur_col = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void move_cursor(void) {
    uint16_t pos = (uint16_t)(cur_row * COLS + cur_col);
    outb(0x3D4, 14);
    outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

static void scroll(void) {
    int r, c;
    for (r = 0; r < ROWS - 1; r++)
        for (c = 0; c < COLS; c++)
            fb[r * COLS + c] = fb[(r + 1) * COLS + c];
    for (c = 0; c < COLS; c++)
        fb[(ROWS - 1) * COLS + c] = 0x0720;
    cur_row = ROWS - 1;
}

void vga_init(void) {
    vga_clear(0x0F);
}

void vga_clear(uint8_t attr) {
    int i;
    for (i = 0; i < COLS * ROWS; i++)
        fb[i] = (uint16_t)((attr << 8) | ' ');
    cur_row = cur_col = 0;
    move_cursor();
}

void vga_putc(char c) {
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == '\r') {
        cur_col = 0;
    } else if (c == '\b') {
        if (cur_col > 0) cur_col--;
        fb[cur_row * COLS + cur_col] = (uint16_t)((cur_attr << 8) | ' ');
    } else {
        fb[cur_row * COLS + cur_col] = (uint16_t)((cur_attr << 8) | (uint8_t)c);
        cur_col++;
    }
    if (cur_col >= COLS) {
        cur_col = 0;
        cur_row++;
    }
    if (cur_row >= ROWS) scroll();
    move_cursor();
}

void vga_puts(const char *s) {
    while (*s) vga_putc(*s++);
}

static void print_num(uint32_t n, int base, int upper, int width, int zero) {
    static const char *digits = "0123456789abcdef";
    char buf[16];
    int i = 0, pad;
    if (n == 0) buf[i++] = '0';
    while (n) {
        char d = digits[n % base];
        if (upper && d >= 'a') d -= 32;
        buf[i++] = d;
        n /= base;
    }
    pad = width - i;
    while (pad-- > 0) vga_putc(zero ? '0' : ' ');
    while (i) vga_putc(buf[--i]);
}

static void vfmt(const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { vga_putc(*fmt++); continue; }
        fmt++;
        /* 可选：'0' + 宽度数字，如 %02u */
        int width = 0, zero = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt++) {
        case 'd': print_num((uint32_t)va_arg(ap, int), 10, 0, width, zero); break;
        case 'u': print_num(va_arg(ap, uint32_t), 10, 0, width, zero); break;
        case 'x': print_num(va_arg(ap, uint32_t), 16, 0, width, zero); break;
        case 'X': print_num(va_arg(ap, uint32_t), 16, 1, width, zero); break;
        case 'c': vga_putc((char)va_arg(ap, int)); break;
        case 's': vga_puts(va_arg(ap, const char *)); break;
        default: vga_putc('%'); vga_putc(fmt[-1]); break;
        }
    }
}

void vga_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfmt(fmt, ap);
    va_end(ap);
}

void vga_row(int row, uint8_t attr, const char *fmt, ...) {
    int saved_r = cur_row, saved_c = cur_col;
    uint8_t saved_attr = cur_attr;
    int i;
    va_list ap;

    for (i = 0; i < COLS; i++)
        fb[row * COLS + i] = (uint16_t)((attr << 8) | ' ');
    cur_attr = attr;
    cur_row = row;
    cur_col = 0;

    va_start(ap, fmt);
    vfmt(fmt, ap);
    va_end(ap);

    cur_row = saved_r;
    cur_col = saved_c;
    cur_attr = saved_attr;
    move_cursor();
}
