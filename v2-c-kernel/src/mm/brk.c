/* mini-os/v2-c-kernel/src/mm/brk.c
 * v0.26#2 用户堆（brk/sbrk）纯逻辑判定（见 brk.h）。 */
#include "brk.h"

uint32_t brk_pages_up(uint32_t old_brk, uint32_t new_brk) {
    if (new_brk <= old_brk) return 0;
    return ((new_brk - old_brk) + 0xFFFu) >> 12;
}

int brk_in_range(uint32_t addr, uint32_t base, uint32_t max) {
    if (base > max) return 0;
    return addr >= base && addr <= max;
}
