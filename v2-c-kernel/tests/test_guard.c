/* mini-os/v2-c-kernel/tests/test_guard.c
 * v0.13 用户栈守卫页判定宿主单元测试：验证 stack_guard_hit 的边界。
 * 布局（mem.h）：BASE=0x80010000, SLOT=0x2000, GUARD=0x1000
 *   pid n 的槽 = [BASE + n*0x2000, +0x2000)；低半页守卫页(命中)，高半页栈页(不命中)
 * v0.15：签名改为 (fault, pid)，只认定"落在 pid 本进程守卫页"的 fault 才是栈溢出。
 * 编译：gcc -Isrc src/guard.c tests/test_guard.c -o tests/build/test_guard */
#include "mem.h"
#include "utest.h"
#include <stdint.h>

int main(void) {
    /* 栈区外：不命中 */
    CHECK(stack_guard_hit(0x8000FFFFu, 0) == 0);   /* 低于栈区 */
    CHECK(stack_guard_hit(0x80030000u, 0) == 0);   /* 栈区终点（含）之后 */
    CHECK(stack_guard_hit(0x00000000u, 0) == 0);   /* 地址 0 */
    CHECK(stack_guard_hit(0xFFFFFFFFu, 0) == 0);   /* 高地址 */

    /* pid 0 槽 [0x80010000, 0x80012000)：守卫 [0x80010000, 0x80011000) */
    CHECK(stack_guard_hit(0x80010000u, 0) == 1);   /* pid0 守卫页起点 */
    CHECK(stack_guard_hit(0x80010001u, 0) == 1);   /* pid0 守卫页内部 */
    CHECK(stack_guard_hit(0x80010FFFu, 0) == 1);   /* pid0 守卫页末字节 */
    CHECK(stack_guard_hit(0x80011000u, 0) == 0);   /* pid0 栈页起点 */
    CHECK(stack_guard_hit(0x80011FFFu, 0) == 0);   /* pid0 栈页顶 */

    /* pid 2 槽 [0x80014000, 0x80016000) */
    CHECK(stack_guard_hit(0x80014000u, 2) == 1);   /* pid2 守卫页起点 */
    CHECK(stack_guard_hit(0x80014FFFu, 2) == 1);   /* pid2 守卫页末字节 */
    CHECK(stack_guard_hit(0x80015000u, 2) == 0);   /* pid2 栈页起点 */
    CHECK(stack_guard_hit(0x80015FFFu, 2) == 0);   /* pid2 栈页顶 */

    /* v0.15 修正回归：同一地址按不同 pid 归属不同槽。
     * 槽顶边界 0x80014000 = pid1 槽顶（栈页顶，非本进程守卫）
     *                = pid2 守卫页起点（本进程守卫）。 */
    CHECK(stack_guard_hit(0x80014000u, 1) == 0);   /* pid1：槽顶边界不是本进程守卫（旧实现误报） */
    CHECK(stack_guard_hit(0x80014000u, 2) == 1);   /* pid2：同一地址是自身守卫页 */
    CHECK(stack_guard_hit(0x80013FFFu, 1) == 0);   /* pid1 栈页顶（不命中） */
    CHECK(stack_guard_hit(0x80013FFFu, 2) == 0);   /* pid2：addr 在自身守卫页之前，不命中 */
    CHECK(stack_guard_hit(0x80012000u, 0) == 0);   /* pid0 槽顶边界不是本进程守卫 */
    CHECK(stack_guard_hit(0x80012000u, 1) == 1);   /* pid1 守卫页起点 */

    /* 栈区终点的最后一槽：pid15 守卫页 [0x8002E000, 0x8002F000) */
    CHECK(stack_guard_hit(0x8002E000u, 15) == 1);  /* pid15 守卫页起点 */
    CHECK(stack_guard_hit(0x8002EFFFu, 15) == 1);  /* pid15 守卫页末字节 */
    CHECK(stack_guard_hit(0x8002F000u, 15) == 0);  /* pid15 栈页起点 */

    UTEST_SUMMARY("test_guard");
}
