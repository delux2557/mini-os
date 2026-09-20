/* mini-os/v2-c-kernel/src/app/logflood.c
 * ring3「日志放大面」洪泛探针：把若干"曾被无界记录"的高发**拒绝路径**各打 N 次，
 * 让测试侧据此断言**日志行数有界**（判据在 tests/qemu_regression.sh，不在本程序）。
 *
 * 背景（实证）：本 PR 之前，循环调未知 syscall 50 次 ⇒ `[user] unknown syscall` **恰好 50 行**、
 * 零限流（对照 0 行；17 次 ⇒ 17 行）。而串口整行输出在 **cli 区间内忙等** THR（见 serial.c 的 K1
 * 说明），洪泛既烧带宽、又抬高行撕裂概率（#193 已量化）⇒ 由 `src/kernel/logthrottle.h` 收成
 * "首次 + 每 LOG_THROTTLE_PERIOD 次"。
 *
 * 为什么断言不写在这里：用户态进程**断言不了自己的日志行数**——那一层正是被内核限流的对象。
 * 本程序只负责①产生"本应无界"的日志量、②自报调用数（供测试侧算上界）。
 *
 * 三段（次数与枚举值固定，测试侧据此算上界 = 段数 × (1 + N/64)）：
 *   ① 未知 syscall(4095) ×200 ⇒ 限流后 4 行（n=1,64,128,192）
 *   ② brk(越界地址)      ×64  ⇒ 2 行（n=1,64）
 *   ③ sem_create(id,-1)  ×64  ⇒ 2 行（n=1,64）
 * 结束打**一行** `[logflood] n1=200 n2=64 n3=64 done`——单次 sys_print（多段 sys_print 会被
 * 并发输出撕碎，整行断言即假红，同 chaos/heapdemo 的教训）。 */
#include "user_lib.h"

#define N1 200u   /* 未知 syscall */
#define N2 64u    /* brk 越界 */
#define N3 64u    /* sem_create 负数 init */

static void app_lit(char *buf, uint32_t *i, const char *s) {
    while (*s) buf[(*i)++] = *s++;
}
static void app_dec(char *buf, uint32_t *i, uint32_t n) {
    uint32_t d[10], nd = 0;
    if (n == 0) d[nd++] = 0;
    while (n) { d[nd++] = n % 10; n /= 10; }
    while (nd) buf[(*i)++] = (char)('0' + d[--nd]);
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* ① 未知号 4095：不在 syscall_table.h（最大 45）⇒ 走 dispatch 的 default: 分支 */
    for (uint32_t i = 0; i < N1; i++) (void)syscall3(4095u, 0, 0, 0);

    /* ② brk 越界地址（0x1000 远低于 heap_base；addr=0 是"查询"语义，不可用） */
    for (uint32_t i = 0; i < N2; i++) (void)syscall3(SYS_BRK, 0x1000u, 0, 0);

    /* ③ sem_create(id=2, init=-1)：负 init 必被拒（SEM-1 fail-closed），且无状态副作用 */
    for (uint32_t i = 0; i < N3; i++) (void)syscall3(SYS_SEM_CREATE, 2u, (uint32_t)-1, 0);

    char buf[64];
    uint32_t i = 0;
    app_lit(buf, &i, "[logflood] n1="); app_dec(buf, &i, N1);
    app_lit(buf, &i, " n2=");           app_dec(buf, &i, N2);
    app_lit(buf, &i, " n3=");           app_dec(buf, &i, N3);
    app_lit(buf, &i, " done\n");
    buf[i] = 0;
    sys_print(buf);
}
