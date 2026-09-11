/* mini-os/v2-c-kernel/src/arch/timer.c
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
    /* NMI 看门狗喂狗：须在 sched_tick 之前（sched_tick 可能切换进程且不返回）。
     * 正常 100Hz 下每 10ms 喂一次，~6s 超时余量充足；cli 死循环时 IRQ0 不再
     * 到达 → 停止喂狗 → QEMU 注入 NMI 突破死循环（见 drv/wdt.c）。 */
    extern void wdt_feed(void);
    wdt_feed();
    /* v0.36（R1.3）：DHCP 租期续约已迁出中断上下文——由 dhcpd 守护进程每
     * 10ms 经 syscall#39 触发 e1000_dhcp_tick（外部审计 A1："策略寄生内核"）。
     * 此处不再在 IRQ0 ISR 里跑续约状态机。 */
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
