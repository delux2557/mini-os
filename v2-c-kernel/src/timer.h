/* mini-os/v2-c-kernel/timer.h */
#ifndef _TIMER_H
#define _TIMER_H
#include <stdint.h>

/* 初始化 PIT 定时器，freq = 每秒中断次数 */
void timer_init(uint32_t freq);
extern volatile uint32_t ticks; /* 从 0 起的中断计数 */

#endif
