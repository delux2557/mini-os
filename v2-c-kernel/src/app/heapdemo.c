/* mini-os/v2-c-kernel/src/app/heapdemo.c
 * v0.26#2 用户堆（brk/sbrk）演示：
 *  - sbrk(0) 查询初始 program break（= USER_HEAP_BASE 0x801A4000）
 *  - sbrk(4096) 扩展一页并写入/校验（内核 [heap] brk 日志、按需补映射）
 *  - sbrk(16384) 再扩 16KB，写入/校验各页
 *  - sys_brk 收缩回 8KB 处（内核保留映射），随后 sbrk 复用已映射页再写入
 *  - 在 brk 之上的极简 bump-allocator 冒烟（编译器 malloc 铺路）
 * 输出约定：每行单次 sys_print（原子行，避免被抢占时拆断日志行）。
 */
#include "user_lib.h"

static char *heap_top;   /* bump-allocator 游标（= 当前 brk 之上可分配区起点） */

/* 极简 bump 分配：对齐 8B，越界按页向 brk 扩；返回 NULL=失败 */
static char *bump_alloc(uint32_t n) {
    uint32_t top = (uint32_t)heap_top;
    uint32_t brk_now = sys_brk(0);
    uint32_t need = n + 8;                       /* 头 8B 记块长（malloc 铺路） */
    if (top + need > brk_now) {
        uint32_t grow = need - (brk_now - top);
        grow = (grow + 0xFFFu) & ~0xFFFu;        /* 按页向上取整 */
        if (sys_sbrk((int32_t)grow) == (uint32_t)-1) return 0;
    }
    *(uint32_t *)top = n;
    heap_top = (char *)(top + need);
    return (char *)(top + 8);
}

static void puthex_nl(const char *tag, uint32_t v) {
    sys_print(tag);
    user_puthex(v);
    sys_print("\n");
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char *p;
    uint32_t i;
    sys_print("[heapdemo] pid=");
    user_putdec(sys_getpid());
    sys_print("\n");

    /* 1) 初始 brk 查询 */
    puthex_nl("[heapdemo] initial brk=", sys_brk(0));

    /* 2) sbrk(4096)：扩一页，写入并校验 */
    p = (char *)sys_sbrk(4096);
    puthex_nl("[heapdemo] sbrk(4096) old=", (uint32_t)p);
    for (i = 0; i < 4096; i++) p[i] = (char)(0xA0 + (i & 0xF));
    for (i = 0; i < 4096; i++)
        if (p[i] != (char)(0xA0 + (i & 0xF))) { sys_print("[heapdemo] page0 verify FAIL\n"); sys_exit(1); }
    sys_print("[heapdemo] 4KB page write+verify OK\n");

    /* 3) sbrk(16384)：再扩 16KB，写入并校验 */
    p = (char *)sys_sbrk(16384);
    puthex_nl("[heapdemo] sbrk(16384) old=", (uint32_t)p);
    for (i = 0; i < 16384; i++) p[i] = (char)(0x30 + (i & 0x3F));
    for (i = 0; i < 16384; i++)
        if (p[i] != (char)(0x30 + (i & 0x3F))) { sys_print("[heapdemo] 16KB verify FAIL\n"); sys_exit(1); }
    sys_print("[heapdemo] 16KB write+verify OK\n");

    /* 4) sys_brk 收缩回 8KB 处（内核保留映射），随后 sbrk 复用已映射页 */
    if (sys_brk((uint32_t)p + 8192) != 0) { sys_print("[heapdemo] brk shrink FAIL\n"); sys_exit(1); }
    p = (char *)sys_sbrk(4096);
    puthex_nl("[heapdemo] after shrink+reuse old=", (uint32_t)p);
    for (i = 0; i < 4096; i++) p[i] = (char)(0xE0 + (i & 0x1F));
    for (i = 0; i < 4096; i++)
        if (p[i] != (char)(0xE0 + (i & 0x1F))) { sys_print("[heapdemo] reuse verify FAIL\n"); sys_exit(1); }
    sys_print("[heapdemo] shrink+reuse write+verify OK\n");

    /* 5) bump-allocator 冒烟：分配 3 块并写入/校验（编译器 malloc 铺路） */
    heap_top = (char *)sys_brk(0);
    {
        char *a = bump_alloc(512);
        char *b = bump_alloc(3000);
        char *c = bump_alloc(64);
        if (!a || !b || !c) { sys_print("[heapdemo] bump_alloc FAIL\n"); sys_exit(1); }
        for (i = 0; i < 512;  i++) a[i] = (char)(i & 0xFF);
        for (i = 0; i < 3000; i++) b[i] = (char)(0x11 + (i & 0x7F));
        for (i = 0; i < 64;   i++) c[i] = (char)(0x99 + (i & 0x0F));
        for (i = 0; i < 512;  i++) if (a[i] != (char)(i & 0xFF)) { sys_print("[heapdemo] blockA FAIL\n"); sys_exit(1); }
        for (i = 0; i < 3000; i++) if (b[i] != (char)(0x11 + (i & 0x7F))) { sys_print("[heapdemo] blockB FAIL\n"); sys_exit(1); }
        for (i = 0; i < 64;   i++) if (c[i] != (char)(0x99 + (i & 0x0F))) { sys_print("[heapdemo] blockC FAIL\n"); sys_exit(1); }
    }
    sys_print("[heapdemo] bump alloc 3 blocks write+verify OK\n");

    puthex_nl("[heapdemo] final brk=", sys_brk(0));
    sys_print("[heapdemo] survived heap brk/sbrk demo\n");
    sys_exit(0);
}
