/* mini-os/v2-c-kernel/tests/test_guard.c
 * v0.13 用户栈守卫页判定宿主单元测试：验证 stack_guard_hit 的边界。
 * 布局（mem.h）：BASE=0x80010000, SLOT=0x2000, GUARD=0x1000
 *   pid n 的槽 = [BASE + n*0x2000, +0x2000)；低半页守卫页(命中)，高半页栈页(不命中)
 * 编译：gcc -Isrc src/guard.c tests/test_guard.c -o tests/build/test_guard */
#include "mem.h"
#include "utest.h"
#include <stdint.h>

int main(void) {
    /* 栈区外：不命中 */
    CHECK(stack_guard_hit(0x8000FFFFu) == 0);   /* 低于栈区 */
    CHECK(stack_guard_hit(0x80030000u) == 0);   /* 栈区终点（含）之后 */
    CHECK(stack_guard_hit(0x00000000u) == 0);   /* 地址 0 */
    CHECK(stack_guard_hit(0xFFFFFFFFu) == 0);   /* 高地址 */

    /* pid 0 槽 [0x80010000, 0x80012000)：守卫 [0x80010000, 0x80011000) */
    CHECK(stack_guard_hit(0x80010000u) == 1);   /* pid0 守卫页起点 */
    CHECK(stack_guard_hit(0x80010001u) == 1);   /* pid0 守卫页内部 */
    CHECK(stack_guard_hit(0x80010FFFu) == 1);   /* pid0 守卫页末字节 */
    CHECK(stack_guard_hit(0x80011000u) == 0);   /* pid0 栈页起点 */
    CHECK(stack_guard_hit(0x80011FFFu) == 0);   /* pid0 栈页顶 */

    /* pid 2 槽 [0x80014000, 0x80016000) */
    CHECK(stack_guard_hit(0x80014000u) == 1);   /* pid2 守卫页起点 */
    CHECK(stack_guard_hit(0x80014FFFu) == 1);   /* pid2 守卫页末字节 */
    CHECK(stack_guard_hit(0x80015000u) == 0);   /* pid2 栈页起点 */
    CHECK(stack_guard_hit(0x80015FFFu) == 0);   /* pid2 栈页顶 */

    /* 相邻槽边界：pid1 栈页 [0x80012000,0x80014000)，pid2 守卫 [0x80014000,..) */
    CHECK(stack_guard_hit(0x80013FFFu) == 0);   /* pid1 栈页顶（不命中） */
    CHECK(stack_guard_hit(0x80014000u) == 1);   /* pid2 守卫页（跨槽边界） */

    UTEST_SUMMARY("test_guard");
}
