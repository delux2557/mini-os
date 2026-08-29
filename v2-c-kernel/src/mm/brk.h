/* mini-os/v2-c-kernel/src/mm/brk.h
 * v0.26#2 用户堆（brk/sbrk）纯逻辑判定：布局边界 + 页扩展跨度。
 * 与硬件解耦，可宿主单测（tests/test_brk.c）。
 * 堆区 [base, max)：base 页对齐，堆顶 brk 向上生长；扩展按页补映射
 * （每页 4KB，old_brk 页对齐），收缩只更新 brk（保留映射、复用不释放）。 */
#ifndef _BRK_H
#define _BRK_H
#include <stdint.h>

/* new_brk > old_brk 时，从 old_brk 对齐起点到 new_brk 之间需处理的页数；
 * new_brk <= old_brk 返回 0（收缩/不动，无新页）。 */
uint32_t brk_pages_up(uint32_t old_brk, uint32_t new_brk);

/* addr 是否落在堆区 [base, max]（含两端）。base<=max 才合法。 */
int brk_in_range(uint32_t addr, uint32_t base, uint32_t max);

#endif
