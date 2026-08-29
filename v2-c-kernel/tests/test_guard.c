/* mini-os/v2-c-kernel/tests/test_guard.c
 * v0.13 用户栈守卫页判定宿主单元测试（v0.26 扩为三态 OK/GROWTH/BOOM）。
 * 布局（mem.h）：BASE=0x80010000, SLOT=0x8000(32KB), GUARD=0x1000, PAGES=7
 *   pid n 的槽 = [BASE + n*0x8000, +0x8000)；槽底 4KB 守卫页永不映射（硬底），
 *   栈从槽顶向下生长，初始只映射顶页（stack_bottom = 槽顶 - 4KB）。
 * 语义：命中"当前守卫页"（栈底下方 1 页，槽内还有空间）-> GROWTH；
 *       深越界/已到硬底 -> BOOM；槽外或已映射区 -> OK。
 * 编译：gcc -Isrc src/guard.c tests/test_guard.c -o tests/build/test_guard */
#include "mem.h"
#include "utest.h"
#include <stdint.h>

int main(void) {
    /* 槽外：一律 OK（非栈事件） */
    CHECK(stack_guard_hit(0x8000FFFFu, 0, 0x80017000u) == STACK_OK);  /* 低于栈区 */
    CHECK(stack_guard_hit(0x80018000u, 0, 0x80017000u) == STACK_OK);  /* pid0 槽顶边界（含）之后 */
    CHECK(stack_guard_hit(0x00000000u, 0, 0x80017000u) == STACK_OK);  /* 地址 0 */
    CHECK(stack_guard_hit(0xFFFFFFFFu, 0, 0x80017000u) == STACK_OK);  /* 高地址 */

    /* pid 0 槽 [0x80010000, 0x80018000)：初始栈底 = 0x80017000 */
    CHECK(stack_guard_hit(0x80017000u, 0, 0x80017000u) == STACK_OK);  /* 已映射栈页起点 */
    CHECK(stack_guard_hit(0x80017FFFu, 0, 0x80017000u) == STACK_OK);  /* 栈页顶 */
    CHECK(stack_guard_hit(0x80016000u, 0, 0x80017000u) == STACK_GROWTH); /* 当前守卫页起点 */
    CHECK(stack_guard_hit(0x80016001u, 0, 0x80017000u) == STACK_GROWTH); /* 守卫页内部 */
    CHECK(stack_guard_hit(0x80016FFFu, 0, 0x80017000u) == STACK_GROWTH); /* 守卫页末字节 */
    CHECK(stack_guard_hit(0x80015FFFu, 0, 0x80017000u) == STACK_BOOM);   /* 深越界 */
    CHECK(stack_guard_hit(0x80010000u, 0, 0x80017000u) == STACK_BOOM);   /* 槽底硬底 */
    CHECK(stack_guard_hit(0x80010FFFu, 0, 0x80017000u) == STACK_BOOM);   /* 硬底守卫页内 */

    /* pid 2 槽 [0x80020000, 0x80028000)：初始栈底 = 0x80027000 */
    CHECK(stack_guard_hit(0x80020000u, 2, 0x80027000u) == STACK_BOOM);   /* 槽底硬底 */
    CHECK(stack_guard_hit(0x80026000u, 2, 0x80027000u) == STACK_GROWTH); /* 当前守卫页起点 */
    CHECK(stack_guard_hit(0x80026FFFu, 2, 0x80027000u) == STACK_GROWTH); /* 守卫页末字节 */
    CHECK(stack_guard_hit(0x80027000u, 2, 0x80027000u) == STACK_OK);     /* 栈页起点 */
    CHECK(stack_guard_hit(0x80027FFFu, 2, 0x80027000u) == STACK_OK);     /* 栈页顶 */

    /* v0.15 修正回归：同一地址按不同 pid 归属不同槽。
     * 0x80018000 = pid0 槽顶边界（槽外，OK）= pid1 槽底硬底（BOOM）。 */
    CHECK(stack_guard_hit(0x80018000u, 0, 0x80017000u) == STACK_OK);   /* pid0：槽顶边界非本进程 */
    CHECK(stack_guard_hit(0x80018000u, 1, 0x8001F000u) == STACK_BOOM); /* pid1：自身槽底硬底 */
    CHECK(stack_guard_hit(0x8001E000u, 1, 0x8001F000u) == STACK_GROWTH);/* pid1：当前守卫页起点 */
    CHECK(stack_guard_hit(0x8001EFFFu, 1, 0x8001F000u) == STACK_GROWTH);/* pid1：守卫页末字节 */
    CHECK(stack_guard_hit(0x8001DFFFu, 1, 0x8001F000u) == STACK_BOOM);  /* pid1：深越界 */
    CHECK(stack_guard_hit(0x80020000u, 1, 0x8001F000u) == STACK_OK);    /* pid1：槽顶边界（槽外） */
    CHECK(stack_guard_hit(0x80020000u, 2, 0x80027000u) == STACK_BOOM);  /* pid2：同一地址是自身硬底 */

    /* v0.26 生长后：栈底下移后守卫页随之下移（逐页生长语义） */
    CHECK(stack_guard_hit(0x8001D000u, 1, 0x8001E000u) == STACK_GROWTH);/* 二次生长：当前守卫页 */
    CHECK(stack_guard_hit(0x8001DFFFu, 1, 0x8001E000u) == STACK_GROWTH);
    CHECK(stack_guard_hit(0x8001CFFFu, 1, 0x8001E000u) == STACK_BOOM);  /* 深越界 */

    /* 已生长到硬底（stack_bottom == 槽底守卫页顶）：无空间，命中守卫页即 BOOM */
    CHECK(stack_guard_hit(0x80011000u, 0, 0x80011000u) == STACK_OK);   /* 最低栈页起点 */
    CHECK(stack_guard_hit(0x80010FFFu, 0, 0x80011000u) == STACK_BOOM); /* 硬底守卫页 */
    CHECK(stack_guard_hit(0x80010000u, 0, 0x80011000u) == STACK_BOOM); /* 槽底 */

    /* 栈区终点最后一槽：pid15 槽 [0x80088000, 0x80090000) */
    CHECK(stack_guard_hit(0x80088000u, 15, 0x8008F000u) == STACK_BOOM);   /* 槽底硬底 */
    CHECK(stack_guard_hit(0x8008E000u, 15, 0x8008F000u) == STACK_GROWTH); /* 当前守卫页起点 */
    CHECK(stack_guard_hit(0x8008F000u, 15, 0x8008F000u) == STACK_OK);     /* 栈页起点 */
    CHECK(stack_guard_hit(0x80090000u, 15, 0x8008F000u) == STACK_OK);     /* 栈区终点之后 */

    UTEST_SUMMARY("test_guard");
}
