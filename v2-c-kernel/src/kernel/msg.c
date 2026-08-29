/* mini-os/v2-c-kernel/msg.c
 * 有界消息队列实现（v0.7）：纯逻辑，无内核依赖，可宿主单测。
 * 设计：环形缓冲 + 两个 FIFO 等待队列。
 *  - send：缓冲有空位则直接入队；否则把 {pid, 消息} 暂存到生产者队列并等待。
 *  - recv：缓冲非空则直接取出；否则把 pid 加入消费者队列等待。
 *  - 唤醒语义（经典"交棒"）：
 *      * 生产成功 → msg_send_wake 若有等待消费者，直接把刚入队的消息交给它
 *        （消费者 recv 直接返回该消息，缓冲不滞留）；
 *      * 消费成功 → msg_recv_wake 若有暂存生产者，把它的消息搬入缓冲并唤醒它
 *        （被唤醒生产者不再 send，由内核代发，send 视为成功）。
 *  唤醒/阻塞的"调度动作"不在本模块，由 syscall 层组合 sched_block/sched_wake 完成。
 */
#include "msg.h"

void msg_init(msgq_t *q, uint32_t capacity) {
    if (capacity > MSG_CAPACITY_MAX) capacity = MSG_CAPACITY_MAX;
    if (capacity == 0) capacity = 1;
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    q->prod_count = 0;
    q->cons_count = 0;
}

uint32_t msg_capacity(msgq_t *q) { return q->capacity; }
uint32_t msg_count(msgq_t *q)    { return q->count; }
uint32_t msg_free(msgq_t *q)     { return q->capacity - q->count; }
uint32_t msg_prod_wait(msgq_t *q){ return q->prod_count; }
uint32_t msg_cons_wait(msgq_t *q){ return q->cons_count; }

/* 环形缓冲：尾插 + 头出（count 维护，支持回绕） */
static void ring_push(msgq_t *q, uint32_t v) {
    uint32_t tail = (q->head + q->count) % q->capacity;
    q->slots[tail] = v;
    q->count++;
}
static uint32_t ring_pop(msgq_t *q) {
    uint32_t v = q->slots[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return v;
}

int msg_send_try(msgq_t *q, uint32_t value, uint32_t pid) {
    if (q->count < q->capacity) {
        ring_push(q, value);
        return 0;                       /* 入队成功，调用方应 msg_send_wake */
    }
    if (q->prod_count >= MSG_MAX_WAITERS)
        return -1;                      /* 生产者等待队列满 */
    q->producers[q->prod_count].pid = pid;
    q->producers[q->prod_count].value = value;
    q->prod_count++;
    return 1;                           /* 应阻塞生产者（消息已暂存） */
}

uint32_t msg_send_wake(msgq_t *q, uint32_t *out_value) {
    if (q->cons_count > 0) {
        /* 缓冲此刻必非空（消费者只在空缓冲时等待），把队首消费者的消息直接取出交付 */
        uint32_t pid = q->consumers[0];
        for (uint32_t i = 1; i < q->cons_count; i++)
            q->consumers[i - 1] = q->consumers[i];
        q->cons_count--;
        *out_value = ring_pop(q);
        return pid;
    }
    return MSG_NO_PID;
}

int msg_recv_try(msgq_t *q, uint32_t *out, uint32_t pid) {
    if (q->count > 0) {
        *out = ring_pop(q);
        return 0;                       /* 取出成功，调用方应 msg_recv_wake */
    }
    if (q->cons_count >= MSG_MAX_WAITERS)
        return -1;                      /* 消费者等待队列满 */
    q->consumers[q->cons_count++] = pid;
    return 1;                           /* 应阻塞消费者 */
}

uint32_t msg_recv_wake(msgq_t *q) {
    if (q->prod_count > 0 && q->count < q->capacity) {
        /* 把队首暂存生产者的消息搬入缓冲并唤醒它（其 send 由内核代发，视为成功） */
        uint32_t pid   = q->producers[0].pid;
        uint32_t value = q->producers[0].value;
        for (uint32_t i = 1; i < q->prod_count; i++)
            q->producers[i - 1] = q->producers[i];
        q->prod_count--;
        ring_push(q, value);
        return pid;
    }
    return MSG_NO_PID;
}
