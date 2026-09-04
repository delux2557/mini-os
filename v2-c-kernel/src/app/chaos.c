/* mini-os/v2-c-kernel/src/app/chaos.c
 * 加固 A-1 ④：ring3 随机指令流 chaos 探针（故障隔离回归）。
 *
 * 目的：反复让"子进程从 ring3 执行随机坏指令"，验证内核把每次 CPU 异常都
 * 隔离到该进程（sched_kill），而 父进程 + 内核整机 讨论绝对存活。这是对
 * SEC-01（用户态 ud2 不再整机停机）的放大化压测：ud2/除零/cli/hlt/lgdt 等
 * 特权或非法指令交错随机选取，连续 N 轮，任何一轮漏杀/波及内核/泄漏资源
 * 都会让父进程无法走完（或审计暴露）。
 *
 * 结构：父进程逐轮 fork 一个子进程，子进程按 LCG 伪随机选一条坏指令执行；
 * 坏指令必触发 #UD/#DE/#BP/#GP 之一 → 内核 panic_dump 打印现场并 sched_kill
 * 该子进程 → 父进程 sys_wait 回收后进入下一轮。所有轮次都回收后父进程打印
 * survived 并以 code=0 干净退出。若某坏指令未被捕获（children 落到打印
 * NOT_TRAPPED），断言即失败。
 */
#include "user_lib.h"

#define CHAOS_ROUNDS 6u

typedef void (*sab_fn)(void);

/* volatile sink：防 -O2 把"不落地的坏指令/消费"优化掉 */
static volatile uint32_t g_sink;

/* 每条坏指令在 ring3 触发一种 CPU 异常：
 *   ud2  -> #UD (6)；int3 -> #BP (3)；cli/hlt/lgdt -> #GP (13)；div0 -> #DE (0) */
static void op_ud2(void)  { __asm__ volatile("ud2");  g_sink = 1; }
static void op_int3(void) { __asm__ volatile("int3"); g_sink = 2; }
static void op_cli(void)  { __asm__ volatile("cli");  g_sink = 3; }
static void op_hlt(void)  { __asm__ volatile("hlt");  g_sink = 4; }
static void op_div0(void) {
    uint32_t a = 1000u, dinit = 0u;
    __asm__ volatile("div %2" : "+a"(a), "+d"(dinit) : "r"(0u));  /* 除零 -> #DE */
    g_sink = a;
}
static void op_lgdt(void) {
    uint16_t fake = 0;
    __asm__ volatile("lgdt %0" : : "m"(fake));                    /* 特权 -> #GP */
    g_sink = 5;
}

static const sab_fn OPS[] = { op_ud2, op_int3, op_cli, op_hlt, op_div0, op_lgdt };
#define OPS_N  ((uint32_t)(sizeof(OPS) / sizeof(OPS[0])))

/* 简易 LCG（规避风险按模选取），随机性只为"交错覆盖多种坏指令" */
static uint32_t rng_state;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t mypid = sys_getpid();
    rng_state = mypid * 0x9E3779B9u + 0xC0FFEE00u;   /** 每启动种子不同 */

    sys_print("[chaos] start pid=");
    user_putdec(mypid);
    sys_print(" rounds=");
    user_putdec(CHAOS_ROUNDS);
    sys_print("\n");

    for (uint32_t k = 0; k < CHAOS_ROUNDS; k++) {
        uint32_t idx = rng_next() % OPS_N;
        sys_print("[chaos] round ");
        user_putdec(k + 1);
        sys_print("/");
        user_putdec(CHAOS_ROUNDS);
        sys_print(" forking op=");
        user_putdec(idx);
        sys_print("\n");

        uint32_t child = sys_fork();
        if (child == 0) {
            /* 子进程：执行坏指令。正常应触发 CPU 异常被内核杀（走不到这里） */
            OPS[idx]();
            /* 若异常未被隔离、执行落到此处 -> 探针失败标记 */
            sys_print("[chaos] op=");
            user_putdec(idx);
            sys_print(" NOT_TRAPPED (FAIL)\n");
            sys_exit(7);
        }
        /* 父进程：回收子进程（sched_kill 路径会唤醒等待的父进程） */
        int status = 0;
        int wr = sys_wait(child, &status);
        if (wr != (int)child) {
            sys_print("[chaos] wait mismatch\n");
            sys_exit(3);
        }
    }

    /* 内核自审计：若任一子进程异常导致资源泄漏（帧/信号量/PCB），这里暴露 */
    uint32_t audit = sys_kern_audit();
    sys_print("[chaos] survived ");
    user_putdec(CHAOS_ROUNDS);
    sys_print(" rounds audit=");
    user_putdec(audit);
    if (audit == 0) sys_print(" (clean)\n");
    else sys_print(" (LEAK)\n");
    sys_exit(audit == 0 ? 0 : 4);
}