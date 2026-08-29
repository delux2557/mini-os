/* mini-os/v2-c-kernel/serial.c
 * 串口 COM1 (0x3F8) 驱动：
 *  - 输出：无图形界面环境下的内核调试输出通道（serial_puts/printf）
 *  - v0.10 输入：IRQ4 接收中断 -> 读走字符 -> 经钩子送入行缓冲（如 kb_feed_char），
 *    使 `qemu -serial stdio` 成为可交互的串口终端（供外部 agent / 真实硬件调试） */
#include "serial.h"
#include "idt.h"
#include <stdarg.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static serial_rx_hook_t rx_hook = 0;

/* 接收数据可用？LSR bit0 = 接收缓冲就绪 */
int serial_rx_ready(void) { return (inb(0x3FD) & 1) != 0; }

/* 读一个接收字符（RBR） */
char serial_getc(void) { return (char)inb(0x3F8); }

/* 注册接收回调：收到字符时以该字符调用（可为 0 关闭转发） */
void serial_set_rx_hook(serial_rx_hook_t fn) { rx_hook = fn; }

/* IRQ4 处理：接收中断触发后，把 FIFO/缓冲里所有可用字符取走转发 */
static void serial_irq(registers_t *r) {
    (void)r;
    while (serial_rx_ready()) {
        char c = serial_getc();
        if (rx_hook) rx_hook(c);
    }
}

void serial_init(void) {
    outb(0x3F9, 0x00); /* 关中断 */
    outb(0x3FB, 0x80); /* DLAB */
    outb(0x3F8, 0x03); /* 38400 波特率 */
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03); /* 8N1 */
    outb(0x3FA, 0xC7); /* FIFO */
    /* v0.10 接收通道：仅开"接收数据可用"中断；PIC 侧由 idt_init 放开 IRQ4 */
    outb(0x3F9, 0x01); /* IER = RX available */
    irq_install_handler(4, serial_irq);
}

void serial_putc(char c) {
    while ((inb(0x3FD) & 0x20) == 0) /* 等发送缓冲空 */ ;
    outb(0x3F8, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

static void sputn(uint32_t n, int base, int upper, int width, int zero) {
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
    while (pad-- > 0) serial_putc(zero ? '0' : ' ');
    while (i) serial_putc(buf[--i]);
}

void serial_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') { serial_putc(*fmt++); continue; }
        fmt++;
        /* 可选：'0' + 宽度数字，如 %02u */
        int width = 0, zero = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt++) {
        case 'd': sputn((uint32_t)va_arg(ap, int), 10, 0, width, zero); break;
        case 'u': sputn(va_arg(ap, uint32_t), 10, 0, width, zero); break;
        case 'x': sputn(va_arg(ap, uint32_t), 16, 0, width, zero); break;
        case 'X': sputn(va_arg(ap, uint32_t), 16, 1, width, zero); break;
        case 'c': serial_putc((char)va_arg(ap, int)); break;
        case 's': serial_puts(va_arg(ap, const char *)); break;
        default: serial_putc('%'); serial_putc(fmt[-1]); break;
        }
    }
    va_end(ap);
}
