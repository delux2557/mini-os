/* mini-os/v2-c-kernel/msg.h
 * 有界消息队列（v0.7）：纯逻辑对象，只做"环形缓冲 + 双等待队列 + 暂存消息"簿记，
 * 不依赖调度器/中断，可在宿主环境编译运行单元测试（tests/test_msg.c）。
 *
 * 与调度器的对接约定（由 syscall 层完成）：
 *  - msg_send_try() 返回 0  -> 消息已入队，调用方随后应调用 msg_send_wake()
 *                              以判断是否有等待消费者可被直接交付；
 *  - msg_send_try() 返回 1  -> 缓冲已满，消息已暂存在等待队列里，调用方应阻塞该生产者
 *                              （被唤醒后其 send 视为成功，消息由内核代发）；
 *  - msg_recv_try() 返回 0  -> 已取出消息，调用方随后应调用 msg_recv_wake()
 *                              以判断是否有暂存生产者可被搬入缓冲并唤醒；
 *  - msg_recv_try() 返回 1  -> 缓冲为空，调用方应阻塞该消费者；
 *  - 两处 -1 均为对应等待队列满（失败）。
 */
#ifndef _MSG_H
#define _MSG_H
#include <stdint.h>

#define MSG_MAX_WAITERS   8     /* 生产者/消费者各最多等待进程数 */
#define MSG_CAPACITY_MAX  8     /* 队列最大容量 */
#define MSG_NO_PID        0xFFFFFFFFu  /* msg_send_wake/recv_wake 无待唤醒者的返回值 */

typedef struct {
    uint32_t pid;                /* 等待者 pid */
    uint32_t value;              /* 生产者阻塞时暂存的消息（缓冲满时排队） */
} msg_wait_entry_t;

typedef struct {
    uint32_t slots[MSG_CAPACITY_MAX]; /* 环形缓冲 */
    uint32_t head;                    /* 读指针 */
    uint32_t count;                   /* 已占用槽数 */
    uint32_t capacity;                /* 容量（1..MSG_CAPACITY_MAX） */
    msg_wait_entry_t producers[MSG_MAX_WAITERS];  /* 缓冲满时阻塞的生产者（含待发消息） */
    uint32_t prod_count;
    uint32_t consumers[MSG_MAX_WAITERS];          /* 缓冲空时阻塞的消费者 pid */
    uint32_t cons_count;
} msgq_t;

void     msg_init(msgq_t *q, uint32_t capacity);
uint32_t msg_capacity(msgq_t *q);
uint32_t msg_count(msgq_t *q);
uint32_t msg_free(msgq_t *q);
uint32_t msg_prod_wait(msgq_t *q);
uint32_t msg_cons_wait(msgq_t *q);

/* 发送尝试：0=入队成功（随后应 msg_send_wake）；1=缓冲满，已暂存、应阻塞生产者；-1=队列满 */
int      msg_send_try(msgq_t *q, uint32_t value, uint32_t pid);
/* 入队成功后调用：若有等待消费者，把缓冲中的消息直接交付给它（经 out_value 返回），
 * 返回其 pid；否则返回 MSG_NO_PID */
uint32_t msg_send_wake(msgq_t *q, uint32_t *out_value);
/* 接收尝试：0=已取出到 *out（随后应 msg_recv_wake）；1=缓冲空，应阻塞消费者；-1=队列满 */
int      msg_recv_try(msgq_t *q, uint32_t *out, uint32_t pid);
/* 取出成功后调用：若有暂存生产者且缓冲有空位，把其消息搬入缓冲并返回其 pid；
 * 否则返回 MSG_NO_PID */
uint32_t msg_recv_wake(msgq_t *q);

#endif
