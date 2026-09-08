/* mini-os/v2-c-kernel/src/drv/wdt.h */
#ifndef _WDT_H
#define _WDT_H

/* NMI 看门狗（i6300esb + QEMU -watchdog-action inject-nmi）。
 * wdt_init 须在分页开启之后、紧跟 timer_init 调用（首 tick 即开始喂狗，
 * 避免引导空隙误报）；wdt_feed 由定时器中断每 tick 调用——cli 段死循环时
 * IRQ0 被屏蔽、喂狗停止，QEMU 超时后注入 NMI 突破死循环。 */
void wdt_init(void);
void wdt_feed(void);

#endif
