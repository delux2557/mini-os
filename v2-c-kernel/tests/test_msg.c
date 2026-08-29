/* mini-os/v2-c-kernel/tests/test_msg.c
 * 消息队列宿主单元测试：只编译 src/msg.c（纯逻辑），
 * 验证环形缓冲、双等待队列 FIFO、生产者阻塞时暂存消息、消费/生产唤醒交棒、边界等。
 */
#include "utest.h"
#include "msg.h"

int main(void) {
    msgq_t q;
    uint32_t outv, wpid;
    uint32_t i;

    /* 1) init：容量正确、空缓冲、无等待者；容量越界/为 0 时收敛 */
    msg_init(&q, 4);
    CHECK_EQ(q.capacity, 4);
    CHECK_EQ(msg_count(&q), 0);
    CHECK_EQ(msg_free(&q), 4);
    CHECK_EQ(msg_prod_wait(&q), 0);
    CHECK_EQ(msg_cons_wait(&q), 0);
    msg_init(&q, 99);
    CHECK_EQ(q.capacity, MSG_CAPACITY_MAX);
    msg_init(&q, 0);
    CHECK_EQ(q.capacity, 1);

    /* 2) send 到未满缓冲：直接入队，FIFO 顺序 */
    msg_init(&q, 4);
    CHECK_EQ(msg_send_try(&q, 10, 1), 0);
    CHECK_EQ(msg_send_try(&q, 20, 2), 0);
    CHECK_EQ(msg_count(&q), 2);
    CHECK_EQ(msg_free(&q), 2);
    CHECK_EQ(msg_cons_wait(&q), 0);
    /* recv 取出，顺序正确 */
    CHECK_EQ(msg_recv_try(&q, &outv, 3), 0);
    CHECK_EQ(outv, 10);
    CHECK_EQ(msg_recv_try(&q, &outv, 3), 0);
    CHECK_EQ(outv, 20);
    CHECK_EQ(msg_count(&q), 0);

    /* 3) recv 空缓冲：返回 1 应阻塞，消费者入队（FIFO） */
    msg_init(&q, 2);
    CHECK_EQ(msg_recv_try(&q, &outv, 10), 1);
    CHECK_EQ(msg_recv_try(&q, &outv, 11), 1);
    CHECK_EQ(msg_cons_wait(&q), 2);
    CHECK_EQ(q.consumers[0], 10);
    CHECK_EQ(q.consumers[1], 11);
    /* 缓冲依旧为空 */
    CHECK_EQ(msg_count(&q), 0);

    /* 4) send 到满缓冲：返回 1 应阻塞生产者，消息被暂存 */
    msg_init(&q, 2);
    CHECK_EQ(msg_send_try(&q, 1, 0), 0);
    CHECK_EQ(msg_send_try(&q, 2, 0), 0);        /* 满 */
    CHECK_EQ(msg_send_try(&q, 3, 10), 1);       /* 生产者 10 阻塞，暂存消息 3 */
    CHECK_EQ(msg_prod_wait(&q), 1);
    CHECK_EQ(q.producers[0].pid, 10);
    CHECK_EQ(q.producers[0].value, 3);
    CHECK_EQ(msg_send_try(&q, 4, 11), 1);       /* 生产者 11 也阻塞，暂存消息 4 */
    CHECK_EQ(msg_prod_wait(&q), 2);
    CHECK_EQ(msg_count(&q), 2);                 /* 缓冲仍满 */

    /* 5) recv 成功后的生产者唤醒：暂存消息被搬入缓冲，返回生产者 pid */
    CHECK_EQ(msg_recv_try(&q, &outv, 20), 0);
    CHECK_EQ(outv, 1);
    wpid = msg_recv_wake(&q);
    CHECK_EQ(wpid, 10);                          /* FIFO：先唤醒生产者 10 */
    CHECK_EQ(msg_prod_wait(&q), 1);
    CHECK_EQ(msg_count(&q), 2);                  /* 消息 3 搬入，缓冲又满 */
    CHECK_EQ(msg_recv_try(&q, &outv, 20), 0);
    CHECK_EQ(outv, 2);
    wpid = msg_recv_wake(&q);
    CHECK_EQ(wpid, 11);                          /* 再唤醒生产者 11 */
    CHECK_EQ(msg_count(&q), 2);
    /* 无暂存生产者时返回 MSG_NO_PID */
    wpid = msg_recv_wake(&q);
    CHECK_EQ(wpid, MSG_NO_PID);

    /* 6) send 成功后的消费者交棒：消息直接交付给等待消费者，缓冲不滞留 */
    msg_init(&q, 4);
    CHECK_EQ(msg_recv_try(&q, &outv, 30), 1);    /* 消费者 30 等在空缓冲上 */
    CHECK_EQ(msg_cons_wait(&q), 1);
    CHECK_EQ(msg_send_try(&q, 77, 31), 0);       /* 生产者发消息 77 */
    wpid = msg_send_wake(&q, &outv);
    CHECK_EQ(wpid, 30);                          /* 交付给消费者 30 */
    CHECK_EQ(outv, 77);                          /* 消费者 recv 直接返回 77 */
    CHECK_EQ(msg_cons_wait(&q), 0);
    CHECK_EQ(msg_count(&q), 0);                  /* 缓冲已被清空（交棒） */
    /* 无等待消费者时返回 MSG_NO_PID */
    wpid = msg_send_wake(&q, &outv);
    CHECK_EQ(wpid, MSG_NO_PID);

    /* 7) 完整有界缓冲互锁场景（含环形回绕）：
     *    容量 2，生产者塞满后阻塞，消费者取走并由内核代发暂存消息 */
    msg_init(&q, 2);
    CHECK_EQ(msg_send_try(&q, 100, 0), 0);
    CHECK_EQ(msg_send_try(&q, 200, 0), 0);      /* 满 */
    CHECK_EQ(msg_send_try(&q, 300, 40), 1);     /* P40 暂存 300 */
    CHECK_EQ(msg_send_try(&q, 400, 41), 1);     /* P41 暂存 400 */
    CHECK_EQ(msg_recv_try(&q, &outv, 50), 0);   /* C50 取 100 */
    CHECK_EQ(outv, 100);
    CHECK_EQ(msg_recv_wake(&q), 40);            /* 唤醒 P40，300 入缓冲 */
    CHECK_EQ(msg_recv_try(&q, &outv, 50), 0);   /* C50 取 200 */
    CHECK_EQ(outv, 200);
    CHECK_EQ(msg_recv_wake(&q), 41);            /* 唤醒 P41，400 入缓冲 */
    CHECK_EQ(msg_recv_try(&q, &outv, 51), 0);   /* C51 取 300 */
    CHECK_EQ(outv, 300);
    CHECK_EQ(msg_recv_wake(&q), MSG_NO_PID);
    CHECK_EQ(msg_recv_try(&q, &outv, 52), 0);   /* C52 取 400 */
    CHECK_EQ(outv, 400);
    CHECK_EQ(msg_count(&q), 0);

    /* 8) 等待队列满：返回 -1，不再入队 */
    msg_init(&q, 1);
    CHECK_EQ(msg_send_try(&q, 1, 0), 0);        /* 满 */
    for (i = 0; i < MSG_MAX_WAITERS; i++)
        CHECK_EQ(msg_send_try(&q, 99, i + 1), 1);
    CHECK_EQ(msg_prod_wait(&q), MSG_MAX_WAITERS);
    CHECK_EQ(msg_send_try(&q, 99, 99), -1);
    CHECK_EQ(msg_prod_wait(&q), MSG_MAX_WAITERS);

    msg_init(&q, 1);
    for (i = 0; i < MSG_MAX_WAITERS; i++)
        CHECK_EQ(msg_recv_try(&q, &outv, i + 1), 1);
    CHECK_EQ(msg_cons_wait(&q), MSG_MAX_WAITERS);
    CHECK_EQ(msg_recv_try(&q, &outv, 99), -1);
    CHECK_EQ(msg_cons_wait(&q), MSG_MAX_WAITERS);

    /* 9) 环形回绕：容量 3 下交错"先取后发"，FIFO 不丢不乱，head 回绕 */
    msg_init(&q, 3);
    for (i = 0; i < 6; i++) {
        if (i >= 3) {
            CHECK_EQ(msg_recv_try(&q, &outv, 0), 0);
            CHECK_EQ(outv, (i - 3) * 10);
        }
        CHECK_EQ(msg_send_try(&q, i * 10, 0), 0);
    }
    while (msg_count(&q) > 0) {
        CHECK_EQ(msg_recv_try(&q, &outv, 0), 0);
    }
    CHECK_EQ(msg_count(&q), 0);

    /* 10) 生产者等待队列满时的消费者交棒不受影响 */
    msg_init(&q, 1);
    CHECK_EQ(msg_send_try(&q, 5, 0), 0);        /* 满 */
    for (i = 0; i < MSG_MAX_WAITERS; i++)
        CHECK_EQ(msg_send_try(&q, 9, i + 1), 1);/* 等待队列满 */
    CHECK_EQ(msg_recv_try(&q, &outv, 60), 0);   /* 消费一条 */
    CHECK_EQ(outv, 5);
    CHECK_EQ(msg_recv_wake(&q), 1);             /* 唤醒队首生产者，其消息入缓冲 */

    UTEST_SUMMARY("test_msg");
}
