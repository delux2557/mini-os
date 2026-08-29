/* mini-os/v2-c-kernel/src/guard.c
 * v0.13 用户栈守卫页判定：纯逻辑（只依赖 mem.h 的布局常量），可宿主单测。
 * 用户栈区按 8KB 槽错开：槽内低 4KB 为守卫页（不映射），高 4KB 为栈页。
 * 栈从槽顶向下增长，下溢越过栈页底部即进入守卫页（地址落在槽内低半页）。
 */
#include "mem.h"
#include <stdint.h>

int stack_guard_hit(uint32_t fault) {
    if (fault < USER_STACK_AREA_BASE || fault >= USER_STACK_AREA_END) return 0;
    return (fault & (USER_STACK_SLOT - 1)) < USER_STACK_GUARD;
}
