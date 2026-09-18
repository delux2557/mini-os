/* mini-os/v2-c-kernel/src/kernel/sem.h
 * 信号量（v0.6）：纯逻辑对象，只做"计数 + 等待队列"簿记，
 * 不依赖调度器/中断，可在宿主环境编译运行单元测试（tests/test_sem.c）。
 *
 * 与调度器的对接约定（由 syscall 层完成）：
 *  - sem_wait_try() 返回 0  -> 已占用资源，调用方继续运行；
 *  - sem_wait_try() 返回 1  -> 资源不足，pid 已入队，调用方应阻塞该进程；
 *  - sem_signal_wake() 返回 SEM_NO_PID -> 无等待者，count 已自增；
 *  - sem_signal_wake() 返回 pid       -> 应唤醒该 pid（等待队列 FIFO）。
 */
#ifndef _SEM_H
#define _SEM_H
#include <stdint.h>

#define SEM_MAX_WAITERS 8
#define SEM_NO_PID      0xFFFFFFFFu   /* 无等待者时 sem_signal_wake 的返回值 */

typedef struct {
    int32_t  count;                    /* 可用资源数（可为负则不允许，保证 >=0 语义） */
    uint32_t waiters[SEM_MAX_WAITERS]; /* 等待者 pid 队列（FIFO） */
    uint32_t wait_count;
} sem_t;

void     sem_init(sem_t *s, int32_t count);
/* P 操作尝试：0=占用成功；1=已入队、应阻塞；-1=等待队列满/失败 */
int      sem_wait_try(sem_t *s, uint32_t pid);
/* V 操作：返回需唤醒的 pid；无等待者时 count++ 并返回 SEM_NO_PID */
uint32_t sem_signal_wake(sem_t *s);
uint32_t sem_wait_count(sem_t *s);
/* 进程死亡回收：摘除队列中该 pid 的全部条目，返回摘除数（0=不在队列）。
 * 语义要点：**排队中的等待者并不持有 token**（token 只在 sem_signal_wake 弹出的那一刻
 * 才交棒），所以摘除死等待者**不丢 token**，故本函数无需动 count。
 * 纯逻辑（本模块不查活跃性）：由调用方在进程退出路径按 pid 调用。 */
int      sem_reap(sem_t *s, uint32_t pid);
/* 不变量审计（v0.21）：count>=0、waiters<=上限、且 count>0 时无等待者
 * （资源空闲而队列有人 = 丢失了一次 signal）。返回 1=成立 / 0=违反。
 * 纯逻辑，无内核依赖，宿主单测与内核自审计共用。 */
int      sem_invariant_ok(const sem_t *s);

#endif
