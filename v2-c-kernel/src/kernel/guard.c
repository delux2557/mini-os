/* mini-os/v2-c-kernel/src/guard.c
 * v0.13 用户栈守卫页判定（v0.26 扩为三态）：纯逻辑（只依赖 mem.h 布局常量），可宿主单测。
 * 用户栈区按 32KB 槽错开（v0.26 起）：槽底 4KB 守卫页永不映射（硬底），
 * 其上 28KB 为可生长栈区，栈从槽顶向下增长，初始只映射槽顶一页。
 * v0.15 修正：仅按槽内对齐模式判定会误报——fault 落在某槽顶端边界
 * （= 下一槽守卫页起点）也会命中低半页模式，却并非本进程的守卫页，
 * 故必须校验 fault 是否落在"pid 本进程"的槽内。
 * v0.26 三态化：
 *   - STACK_GROWTH：fault 命中"当前守卫页"（= 当前栈页 stack_bottom 下方 1 页，
 *     且槽内还有生长空间）-> pf_handler 补映射、守卫页下移；
 *   - STACK_BOOM：  fault 越过当前守卫页再往下（深越界），或已生长到槽底硬底
 *     （stack_bottom == hard_floor）仍下探 -> 栈溢出；
 *   - STACK_OK：    fault 不在本进程槽内，或在已映射栈页内（非栈事件）。
 */
#include "mem.h"
#include <stdint.h>

stack_evt_t stack_guard_hit(uint32_t fault, uint32_t pid, uint32_t stack_bottom) {
    uint32_t slot_start = USER_STACK_AREA_BASE + pid * USER_STACK_SLOT;
    uint32_t slot_end   = slot_start + USER_STACK_SLOT;
    if (fault < slot_start || fault >= slot_end) return STACK_OK;  /* 槽外：非栈事件 */
    uint32_t hard_floor = slot_start + USER_STACK_GUARD;           /* 槽底守卫页顶 */
    if (fault >= stack_bottom) return STACK_OK;                    /* 已映射栈页内 */
    if (stack_bottom > hard_floor &&
        fault >= stack_bottom - USER_STACK_GUARD)
        return STACK_GROWTH;                                       /* 当前守卫页：可生长 */
    return STACK_BOOM;                                             /* 深越界 / 无空间 */
}
