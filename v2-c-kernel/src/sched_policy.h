/* mini-os/v2-c-kernel/sched_policy.h
 * 调度策略（纯逻辑，无内核依赖，可宿主单测）：
 *  - 定长环形就绪队列：push / pop / remove / contains / count / empty
 *  - 约定：运行中的进程不在队列中，只有"可运行但未运行"的进程在队列中；
 *    入队即追加队尾(FIFO)，出队取队头 —— 配合该约定即构成轮转(RR)调度。
 */
#ifndef _SCHED_POLICY_H
#define _SCHED_POLICY_H
#include <stdint.h>

#define POLICY_MAX_READY 16
#define POLICY_NO_PID    0xFFFFFFFFu   /* 空队列出队返回值 */

typedef struct {
    uint32_t ready[POLICY_MAX_READY];  /* 环形数组，存 pid */
    uint32_t head;                     /* 队头：下次出队位置 */
    uint32_t tail;                     /* 队尾：下次入队位置 */
    uint32_t count;                    /* 当前元素个数 */
} policy_readyq_t;

void     policy_readyq_init(policy_readyq_t *q);
/* 入队（队尾）。队列满返回 -1，成功返回 0 */
int      policy_readyq_push(policy_readyq_t *q, uint32_t pid);
/* 出队（队头）。空队列返回 POLICY_NO_PID */
uint32_t policy_readyq_pop(policy_readyq_t *q);
/* 查看队头但不出队。空队列返回 POLICY_NO_PID */
uint32_t policy_readyq_peek(policy_readyq_t *q);
/* 按 pid 移除（把后续元素前移，保持环形语义）。不存在返回 -1 */
int      policy_readyq_remove(policy_readyq_t *q, uint32_t pid);
int      policy_readyq_contains(policy_readyq_t *q, uint32_t pid);
uint32_t policy_readyq_count(policy_readyq_t *q);
int      policy_readyq_empty(policy_readyq_t *q);

#endif
