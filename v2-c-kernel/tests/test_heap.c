/* mini-os/v2-c-kernel/tests/test_heap.c
 * 内核堆分配器宿主单元测试：
 *  - mock frame_alloc/frame_free（用静态内存池代替物理帧）
 *  - 静默 serial_printf
 *  - 验证：对齐、不重叠、分裂、合并、复用、页增长、OOM
 * 编译：gcc -I. -Itests heap.c tests/test_heap.c -o tests/build/test_heap */
#include "heap.h"
#include "utest.h"
#include <stdint.h>
#include <string.h>

/* ---- 外部依赖的宿主替身 ---- */
#define POOL_PAGES 16
static uint8_t page_pool[POOL_PAGES][4096] __attribute__((aligned(16)));
static int pool_next = 0;

/* mem.h 声明，这里实现：返回池中下一页的物理地址 */
uint32_t frame_alloc(void) {
    if (pool_next >= POOL_PAGES) return 0;   /* 模拟内存耗尽 */
    return (uint32_t)(uintptr_t)page_pool[pool_next++];
}
/* v0.9：按连续 count 页分配（池为连续静态数组，天然连续） */
uint32_t frame_alloc_run(uint32_t count) {
    if (pool_next + count > POOL_PAGES) return 0;
    uint32_t first = (uint32_t)(uintptr_t)page_pool[pool_next];
    pool_next += count;
    return first;
}
void frame_free(uint32_t phys) { (void)phys; }

/* serial.h 声明，宿主测试里静默 */
void serial_printf(const char *fmt, ...) { (void)fmt; }

int main(void) {
    /* 1) 初始化与页计数 */
    heap_init();
    CHECK_EQ(heap_page_count(), 0u);

    /* 2) 基本分配：非空 + 8 字节对齐 */
    void *p1 = kmalloc(100);
    CHECK(p1 != 0);
    CHECK_EQ(((uintptr_t)p1 & 7u), 0u);

    /* 3) 多个分配互不重叠 */
    void *a = kmalloc(64);
    void *b = kmalloc(64);
    void *c = kmalloc(64);
    CHECK(a && b && c);
    CHECK(a != b && b != c && a != c);
    /* 写入并读回，确认可寻址 */
    memset(a, 0xAA, 64); memset(b, 0xBB, 64); memset(c, 0xCC, 64);
    CHECK_EQ(*(volatile uint8_t *)a, 0xAA);
    CHECK_EQ(*(volatile uint8_t *)b, 0xBB);
    CHECK_EQ(*(volatile uint8_t *)c, 0xCC);

    /* 4) 分页：单次分配超过当前剩余空间时应补页 */
    uint32_t pages_before = heap_page_count();
    void *big = kmalloc(4000);   /* 超过 page1 剩余(3720)，应占用新页 */
    CHECK(big != 0);
    CHECK(heap_page_count() > pages_before);

    /* 5) 释放后内存可复用（首次适配，不保证原地址） */
    kfree(a);
    void *a2 = kmalloc(64);
    CHECK(a2 != 0);
    CHECK(a2 != b && a2 != c && a2 != big);   /* 不与仍存活块重叠 */

    /* 6) 合并：释放相邻块后可分配更大的块 */
    kfree(b);
    kfree(c);
    /* 相邻 64+64 合并成 144 字节空闲块 */
    void *m = kmalloc(144);
    CHECK(m != 0);

    /* 7) kmalloc(0) 视为 1 字节 */
    void *z = kmalloc(0);
    CHECK(z != 0);

    /* 8) 非法指针释放不崩溃（magic 校验拦截）。
     * 注意：不能释放任意地址（宿主上会解引用未映射内存），
     * 这里指向"池内已分配块内部"来触发魔法数校验。 */
    kfree(0);                       /* NULL，直接返回 */
    void *bad = kmalloc(64);
    kfree((char *)bad + 8);         /* 块内偏移 -> 魔法数非法，应被拦截 */
    kfree(bad);                     /* 正常释放 */

    /* 9) OOM：耗尽所有页后应返回 NULL（而非死循环/崩溃） */
    void *blocks[64];
    int n = 0;
    while (n < 64) {
        void *q = kmalloc(2048);
        if (!q) break;
        blocks[n++] = q;
    }
    CHECK(n < 64);                          /* 最终应分配失败 */
    CHECK_EQ(heap_page_count(), (uint32_t)POOL_PAGES); /* 池已耗尽 */

    /* 10) 释放全部，堆应重新可用 */
    for (int i = 0; i < n; i++) kfree(blocks[i]);
    void *reuse = kmalloc(500);
    CHECK(reuse != 0);

    UTEST_SUMMARY("heap");
}
