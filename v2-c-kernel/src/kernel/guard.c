/* mini-os/v2-c-kernel/src/guard.c
 * v0.13 用户栈守卫页判定：纯逻辑（只依赖 mem.h 的布局常量），可宿主单测。
 * 用户栈区按 8KB 槽错开：槽内低 4KB 为守卫页（不映射），高 4KB 为栈页。
 * 栈从槽顶向下增长，下溢越过栈页底部即进入守卫页（地址落在槽内低半页）。
 * v0.15 修正：仅按槽内对齐模式判定会误报——fault 落在某槽顶端边界
 * （= 下一槽守卫页起点）也会命中低半页模式，却并非本进程的守卫页。
 * 现改为校验 fault 确实落在"pid 本进程"的守卫页 [BASE+pid*SLOT, +GUARD) 内。
 */
#include "mem.h"
#include <stdint.h>

int stack_guard_hit(uint32_t fault, uint32_t pid) {
    uint32_t slot_start = USER_STACK_AREA_BASE + pid * USER_STACK_SLOT;
    if (fault < slot_start || fault >= slot_start + USER_STACK_GUARD) return 0;
    return 1;
}
