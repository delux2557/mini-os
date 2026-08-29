/* mini-os/v2-c-kernel/tests/test_kb.c
 * PS/2 键盘驱动宿主单元测试：
 *  - 直接注入扫描码到 kb_feed_scan，经 kb_getchar 取回
 *  - 验证：普通键、Shift 组合、容量/顺序、环形回绕、满时丢弃、不可打印键
 * 编译：gcc -I. -Itests kb.c tests/test_kb.c -o tests/build/test_kb */
#include "kb.h"
#include "idt.h"
#include "utest.h"
#include <stdint.h>

/* idt.h 声明的 IRQ 注册：宿主测试中无需真正挂中断 */
void irq_install_handler(int irq, isr_t handler) { (void)irq; (void)handler; }

#define SCAN_A   0x1E   /* 'a' 键 */
#define SCAN_S   0x1F   /* 's' 键 */
#define SCAN_1   0x02   /* '1' 键 */
#define SHIFT_L  0x2A   /* 左 Shift 按下 */
#define SHIFT_R  0x36   /* 右 Shift 按下 */
#define SHIFT_LU 0xAA   /* 左 Shift 松开 */
#define SHIFT_RU 0xB6   /* 右 Shift 松开 */
#define KB_FULL  255    /* 环形缓冲最大容量（head 不能追上 tail） */

static void drain_all(void) {
    int c; while ((c = kb_getchar()) != -1) {}
    kb_line_reset();   /* 丢弃上一个用例残留的未完成行 */
}

/* 行缓冲测试：回调计数 */
static int hook_count = 0;
static void line_hook_test(void) { hook_count++; }

int main(void) {
    /* 1) 普通按键入队 */
    CHECK_EQ(kb_feed_scan(SCAN_A), 1);
    CHECK_EQ(kb_getchar(), 'a');

    /* 2) 无输入时返回 -1 */
    CHECK_EQ(kb_getchar(), -1);

    /* 3) Shift 组合：按下左 Shift + 'a' -> 'A' */
    CHECK_EQ(kb_feed_scan(SHIFT_L), 0);
    CHECK_EQ(kb_feed_scan(SCAN_A), 1);
    CHECK_EQ(kb_getchar(), 'A');
    CHECK_EQ(kb_feed_scan(SHIFT_LU), 0);
    CHECK_EQ(kb_feed_scan(SCAN_A), 1);
    CHECK_EQ(kb_getchar(), 'a');            /* Shift 松开后恢复小写 */

    /* 4) 右 Shift 同样生效 */
    CHECK_EQ(kb_feed_scan(SHIFT_R), 0);
    CHECK_EQ(kb_feed_scan(SCAN_1), 1);
    CHECK_EQ(kb_getchar(), '!');
    CHECK_EQ(kb_feed_scan(SHIFT_RU), 0);

    /* 5) 不可打印键（如 F1 等映射为 0）不入队 */
    CHECK_EQ(kb_feed_scan(0x3B), 0);        /* F1 */
    CHECK_EQ(kb_getchar(), -1);

    /* 6) 容量与顺序：填满 255 个交替键，按序取回 */
    drain_all();
    int n = 0;
    while (n < KB_FULL) { kb_feed_scan((n & 1) ? SCAN_S : SCAN_A); n++; }
    CHECK_EQ(kb_feed_scan(SCAN_A), 0);      /* 已满，第 256 个被丢弃 */
    for (int i = 0; i < KB_FULL; i++)
        CHECK_EQ(kb_getchar(), (i & 1) ? 's' : 'a');
    CHECK_EQ(kb_getchar(), -1);             /* 取空 */

    /* 7) 环形回绕：满时取 1 再塞 1，head 回绕到 0 且不丢不坏 */
    drain_all();
    n = 0;
    while (n < KB_FULL) { kb_feed_scan(SCAN_A); n++; }
    CHECK(kb_getchar() != -1);              /* 取走 1 个 */
    CHECK_EQ(kb_feed_scan(SCAN_A), 1);      /* 再塞 1 个 -> head 回绕 */
    int back = 0;
    while (kb_getchar() != -1) back++;
    CHECK_EQ(back, KB_FULL);            /* 剩余数量正确（取 1 又补 1） */

    /* ---- v0.9 行缓冲 ---- */
    /* 8) 组装一行：h,i + 回车 -> 行就绪，取回 "hi" */
    drain_all();
    CHECK_EQ(kb_line_ready(), 0);
    kb_feed_scan(0x23);                 /* 'h' */
    kb_feed_scan(0x17);                 /* 'i' */
    CHECK_EQ(kb_line_ready(), 0);       /* 未回车，尚未就绪 */
    kb_feed_scan(0x1C);                 /* Enter */
    CHECK_EQ(kb_line_ready(), 1);
    char line[16];
    CHECK_EQ(kb_line_take(line, 16), 2);
    CHECK_EQ(line[0] == 'h' && line[1] == 'i' && line[2] == 0, 1);
    CHECK_EQ(kb_line_ready(), 0);       /* 取走后复位 */

    /* 9) 退格删除行尾字符：hi + 退格 + 回车 -> "h" */
    kb_feed_scan(0x23);                 /* 'h' */
    kb_feed_scan(0x17);                 /* 'i' */
    kb_feed_scan(0x0E);                 /* Backspace */
    kb_feed_scan(0x1C);                 /* Enter */
    CHECK_EQ(kb_line_take(line, 16), 1);
    CHECK_EQ(line[0] == 'h' && line[1] == 0, 1);

    /* 10) 行长上限：超过 KB_LINE_MAX 的字符被丢弃，行不越界 */
    drain_all();
    char bigline[KB_LINE_MAX + 1];
    for (int i = 0; i < KB_LINE_MAX + 20; i++) kb_feed_scan(SCAN_A);
    kb_feed_scan(0x1C);                 /* Enter */
    CHECK_EQ(kb_line_take(bigline, sizeof(bigline)), KB_LINE_MAX);

    /* 11) 行完成回调：回车触发一次 */
    hook_count = 0;
    kb_set_line_hook(line_hook_test);
    kb_feed_scan(SCAN_A);
    CHECK_EQ(hook_count, 0);            /* 未回车不触发 */
    kb_feed_scan(0x1C);
    CHECK_EQ(hook_count, 1);
    kb_set_line_hook(0);                /* 解除回调 */
    CHECK_EQ(kb_line_take(line, sizeof(line)), 1);  /* 取走，清理状态 */

    /* 12) 已有未取行时，新回车不覆盖、不重复触发 */
    hook_count = 0;
    kb_set_line_hook(line_hook_test);
    kb_feed_scan(SCAN_A);
    kb_feed_scan(0x1C);                 /* 完成 "a" */
    kb_feed_scan(0x1C);                 /* 未取走前的第二次回车：忽略 */
    CHECK_EQ(hook_count, 1);
    CHECK_EQ(kb_line_take(line, sizeof(line)), 1);
    CHECK_EQ(line[0], 'a');
    kb_set_line_hook(0);

    UTEST_SUMMARY("kb");
}
