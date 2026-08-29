/* mini-os/v2-c-kernel/timer.c
 * PIT(8254) 定时器驱动：产生周期性时钟中断，作为"心跳" */
#include "timer.h"
#include "idt.h"
#include <stdint.h>

volatile uint32_t ticks = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void timer_cb(registers_t *r) {
    ticks++;
    /* 交由调度器：唤醒到期进程 + 抢占切换（可能不返回） */
    extern void sched_tick(registers_t *);
    sched_tick(r);
}

void timer_init(uint32_t freq) {
    uint32_t divisor = 1193182 / freq; /* PIT 基准频率 1.193182 MHz */
    outb(0x43, 0x36);                 /* 通道 0，低/高字节，方波 */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    irq_install_handler(0, timer_cb);
}
