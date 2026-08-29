/* mini-os/v2-c-kernel/sem.c
 * 信号量实现（v0.6）：纯逻辑，无内核依赖，可宿主单测。
 * 采用"计数 + 环形前移等待队列"：
 *  - wait 时若 count>0 直接递减；否则把 pid 追加到等待队列队尾；
 *  - signal 时若队列非空，取出队首 pid 交给调度器唤醒；否则 count++。
 * 唤醒/阻塞的"调度动作"不在本模块，由 syscall 层组合 sched_block/sched_wake 完成。
 */
#include "sem.h"

void sem_init(sem_t *s, int32_t count) {
    s->count = count;
    s->wait_count = 0;
}

int sem_wait_try(sem_t *s, uint32_t pid) {
    if (s->count > 0) {
        s->count--;
        return 0;                       /* 占用成功 */
    }
    if (s->wait_count >= SEM_MAX_WAITERS)
        return -1;                      /* 等待队列满：调用方应视为失败 */
    s->waiters[s->wait_count++] = pid;  /* 入队等待（FIFO 队尾） */
    return 1;                           /* 应阻塞 */
}

uint32_t sem_signal_wake(sem_t *s) {
    if (s->wait_count > 0) {
        uint32_t pid = s->waiters[0];
        for (uint32_t i = 1; i < s->wait_count; i++)
            s->waiters[i - 1] = s->waiters[i];
        s->wait_count--;
        return pid;                     /* 唤排队首等待者，count 不变 */
    }
    s->count++;                         /* 无人等待：释放一份资源 */
    return SEM_NO_PID;
}

uint32_t sem_wait_count(sem_t *s) { return s->wait_count; }
