/* mini-os/v2-c-kernel/src/kernel/logthrottle.h
 * 日志限流：把"每次调用必打"的高发**拒绝日志**收成"**首次 + 每 N 次**"，防 ring3 洪泛把串口刷爆。
 *
 * 为什么需要（实证，非推测）：
 *   · ring3 可**无界**触发若干拒绝日志且无任何限流。实测（guest 内 minicc 编译的探针）：
 *     循环调未知 syscall 50 次 ⇒ `[user] unknown syscall` **恰好 50 行**（对照 0 行，17 次 ⇒ 17 行）。
 *   · 串口整行输出在 **cli 区间内忙等 THR**（K1/BUG-051；代码注释自标：真机/慢后端下 80 字符行
 *     @38400 ≈ 21ms 关中断）⇒ 洪泛既烧带宽、又屏蔽 IRQ0 扰动时间基准。
 *   · 用户进程**多段 sys_print** 拼出的行会被并发输出撕碎（#193 已量化）⇒ 洪泛抬高撕裂概率、污染门禁。
 *   · 同项目的 #191 已对分片日志做"首次 + 每 64 次"（`src/net/netsock.c`）——本文件把那套口径抽成
 *     共享件，避免各站点各写各的。
 *
 * 语义（与 #191 同口径）：命中 = **第 1 次** 或 **第 N 次**（N 的整数倍）；其余静默。
 *   调用方在命中时打印，并把 `t->n`（累计次数）一并打出 ⇒ 抑制量**可见**，不是静默失真。
 *   `period == 0` 显式表示"不限流"（不除零）。
 *
 * 取舍（如实登记）：
 *   · 限流状态是**按站点**（static，非按 pid）⇒ 某进程洪泛会连累他进程的同类日志被抑制。
 *     接受理由：被抑制的是"同一站点的高频重复"，且累计次数仍逐条可见；本内核只有显式设过掩码的
 *     进程才会走 masked 那条（当前仅 sandboxdemo），实际影响面极小。
 *   · 本件只覆盖**拒绝/失败**路径；成功路径的逐调用日志（如 `[net] recvfrom`、`[fs] open/read`）
 *     仍在——见 changelog 的"未覆盖"。
 *
 * 并发：单 CPU 教学内核，调用点都在 syscall 上下文（不被打断），无需原子操作。
 * 头文件内联：不为 10 行逻辑新增 .o 与构建规则；每个站点声明自己的 static 状态
 *   （`static inline` 未使用时不触发 -Wunused-function，符合本仓 -Werror）。
 */
#ifndef MINIOS_LOGTHROTTLE_H
#define MINIOS_LOGTHROTTLE_H

#include <stdint.h>

/* 默认周期：首次 + 每 64 次（与 #191 F5 分片日志一致）。 */
#define LOG_THROTTLE_PERIOD 64u

typedef struct { uint32_t n; } log_throttle_t;

/* 记一次调用；返回 1 = **本次应打印**。调用方随后可用 t->n 打印累计次数。 */
static inline int log_throttle_hit(log_throttle_t *t, uint32_t period) {
    t->n++;
    if (period == 0u) return 1;                 /* 0 ⇒ 不限流（显式，防除零） */
    return (t->n == 1u) || ((t->n % period) == 0u);
}

#endif /* MINIOS_LOGTHROTTLE_H */
