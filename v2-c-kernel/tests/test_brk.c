/* mini-os/v2-c-kernel/tests/test_brk.c
 * v0.26#2 用户堆（brk）纯逻辑宿主单元测试。
 * 覆盖 src/mm/brk.c：brk_pages_up（扩展页跨度）与 brk_in_range（布局边界），
 * 并配合 mem.h 的 USER_HEAP_BASE/MAX 常量做真实布局边界断言。
 * 编译：gcc -Isrc src/mm/brk.c tests/test_brk.c -o tests/build/test_brk */
#include "brk.h"
#include "mem.h"
#include "utest.h"
#include <stdint.h>

int main(void) {
    uint32_t base = USER_HEAP_BASE;   /* 0x801A4000（v0.26#3 重排后的堆区起点） */
    uint32_t max  = USER_HEAP_MAX;    /* 0x801F4000 */

    /* ---- brk_pages_up：扩展需处理页数（old_brk 页对齐） ---- */
    CHECK_EQ(brk_pages_up(base, base), 0u);            /* 不动 */
    CHECK_EQ(brk_pages_up(base + 8192, base), 0u);     /* 收缩：0 新页 */
    CHECK_EQ(brk_pages_up(base, base + 1), 1u);        /* 页内 1 字节也占整页 */
    CHECK_EQ(brk_pages_up(base, base + 4096), 1u);     /* 恰好一页 */
    CHECK_EQ(brk_pages_up(base, base + 4097), 2u);     /* 跨到下一页 */
    CHECK_EQ(brk_pages_up(base, base + 8192), 2u);     /* 两页 */
    CHECK_EQ(brk_pages_up(base, base + 16384), 4u);    /* 四页 */
    CHECK_EQ(brk_pages_up(base, base + 16385), 5u);    /* 四页 + 页内 */

    /* 全堆区跨度 = USER_HEAP_PAGES */
    CHECK_EQ(brk_pages_up(base, max), (uint32_t)((max - base) / 4096));
    CHECK_EQ((max - base) / 4096, (uint32_t)USER_HEAP_PAGES); /* 布局一致：正好 64 页 */

    /* ---- brk_in_range：地址落在 [base, max] 才合法 ---- */
    CHECK(brk_in_range(base, base, max));              /* 起点 */
    CHECK(brk_in_range(max, base, max));               /* 终点（含） */
    CHECK(brk_in_range(base + 4096, base, max));       /* 堆内 */
    CHECK(brk_in_range(base + (uint32_t)USER_HEAP_PAGES * 4096, base, max)); /* 顶页 */
    CHECK(!brk_in_range(base - 1, base, max));         /* 起点之下 */
    CHECK(!brk_in_range(max + 1, base, max));          /* 终点之上（越界被拒） */
    CHECK(!brk_in_range(0, base, max));                /* 地址 0 */
    CHECK(!brk_in_range(0xFFFFFFFFu, base, max));      /* 高地址/负数 */
    CHECK(!brk_in_range(base, max, base));             /* 非法区间 base>max */

    /* 与相邻布局区不重叠：堆起点应在共享内存区之后 */
    CHECK(base >= SHMEM_VBASE + SHMEM_SLOTS * 0x1000u);
    /* 与栈区无冲突：堆区不与任何栈槽重叠 */
    CHECK(base >= USER_STACK_AREA_END);

    UTEST_SUMMARY("test_brk");
}
