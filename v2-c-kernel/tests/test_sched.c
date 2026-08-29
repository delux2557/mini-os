/* mini-os/v2-c-kernel/tests/test_sched.c
 * 调度策略宿主单元测试：只编译 sched_policy.c（纯逻辑），
 * 验证环形就绪队列的 FIFO 轮转语义、满/空边界、remove/contains 等。
 */
#include "utest.h"
#include "sched_policy.h"

int main(void) {
    policy_readyq_t q;
    uint32_t i;

    /* 1) init 后为空 */
    policy_readyq_init(&q);
    CHECK(policy_readyq_empty(&q));
    CHECK_EQ(policy_readyq_count(&q), 0);
    CHECK_EQ(policy_readyq_pop(&q), POLICY_NO_PID);
    CHECK_EQ(policy_readyq_peek(&q), POLICY_NO_PID);

    /* 2) 基本 FIFO：按入队顺序出队 */
    policy_readyq_init(&q);
    for (i = 1; i <= 5; i++) CHECK_EQ(policy_readyq_push(&q, i), 0);
    CHECK_EQ(policy_readyq_count(&q), 5);
    CHECK_EQ(policy_readyq_peek(&q), 1);   /* peek 不改变队列 */
    CHECK_EQ(policy_readyq_count(&q), 5);
    for (i = 1; i <= 5; i++) CHECK_EQ(policy_readyq_pop(&q), i);
    CHECK(policy_readyq_empty(&q));

    /* 3) 满队列：超过 POLICY_MAX_READY 拒绝入队 */
    policy_readyq_init(&q);
    for (i = 0; i < POLICY_MAX_READY; i++) CHECK_EQ(policy_readyq_push(&q, i), 0);
    CHECK_EQ(policy_readyq_push(&q, 99), -1);           /* 满 */
    CHECK_EQ(policy_readyq_count(&q), POLICY_MAX_READY);
    /* 出队一个后可再入队 */
    CHECK_EQ(policy_readyq_pop(&q), 0);
    CHECK_EQ(policy_readyq_push(&q, 99), 0);
    CHECK_EQ(policy_readyq_count(&q), POLICY_MAX_READY);

    /* 4) 环形回绕：head/tail 越过数组末端仍保持 FIFO 顺序 */
    policy_readyq_init(&q);
    /* 填满再清空，迫使 head 回绕到 0 之外 */
    for (i = 0; i < POLICY_MAX_READY; i++) policy_readyq_push(&q, i * 10);
    for (i = 0; i < POLICY_MAX_READY; i++) policy_readyq_pop(&q);
    /* 现在 head == tail == 0；再入队一半，验证顺序 */
    for (i = 0; i < 8; i++) CHECK_EQ(policy_readyq_push(&q, i + 100), 0);
    for (i = 0; i < 8; i++) CHECK_EQ(policy_readyq_pop(&q), i + 100);
    CHECK(policy_readyq_empty(&q));

    /* 5) remove：删除队头 / 队中 / 队尾，不存在返回 -1 */
    policy_readyq_init(&q);
    for (i = 1; i <= 5; i++) policy_readyq_push(&q, i);
    CHECK_EQ(policy_readyq_remove(&q, 3), 0);   /* 队中 */
    CHECK_EQ(policy_readyq_count(&q), 4);
    CHECK_EQ(policy_readyq_pop(&q), 1);          /* 前移后顺序仍正确 */
    CHECK_EQ(policy_readyq_pop(&q), 2);
    CHECK_EQ(policy_readyq_pop(&q), 4);
    CHECK_EQ(policy_readyq_pop(&q), 5);
    CHECK(policy_readyq_empty(&q));

    policy_readyq_init(&q);
    for (i = 1; i <= 3; i++) policy_readyq_push(&q, i);
    CHECK_EQ(policy_readyq_remove(&q, 1), 0);   /* 队头 */
    CHECK_EQ(policy_readyq_pop(&q), 2);
    CHECK_EQ(policy_readyq_remove(&q, 3), 0);   /* 队尾 */
    CHECK(policy_readyq_empty(&q));
    CHECK_EQ(policy_readyq_remove(&q, 42), -1); /* 不存在 */

    /* 6) remove 后仍可继续入队（tail 回退正确） */
    policy_readyq_init(&q);
    for (i = 1; i <= 4; i++) policy_readyq_push(&q, i);
    CHECK_EQ(policy_readyq_remove(&q, 2), 0);
    CHECK_EQ(policy_readyq_push(&q, 50), 0);
    CHECK_EQ(policy_readyq_count(&q), 4);
    CHECK_EQ(policy_readyq_pop(&q), 1);
    CHECK_EQ(policy_readyq_pop(&q), 3);
    CHECK_EQ(policy_readyq_pop(&q), 4);
    CHECK_EQ(policy_readyq_pop(&q), 50);

    /* 7) contains */
    policy_readyq_init(&q);
    for (i = 10; i <= 12; i++) policy_readyq_push(&q, i);
    CHECK(policy_readyq_contains(&q, 11));
    CHECK(!policy_readyq_contains(&q, 99));
    CHECK_EQ(policy_readyq_remove(&q, 11), 0);
    CHECK(!policy_readyq_contains(&q, 11));

    /* 8) 入队顺序即轮转顺序：模拟 A/B/C 三个进程轮转 */
    policy_readyq_init(&q);
    policy_readyq_push(&q, 1);
    policy_readyq_push(&q, 2);
    policy_readyq_push(&q, 3);
    for (i = 0; i < 3; i++) {
        uint32_t n = policy_readyq_pop(&q);        /* 运行 */
        CHECK_EQ(n, 1 + i % 3);
        policy_readyq_push(&q, n);                 /* 让出回队尾 */
    }
    CHECK_EQ(policy_readyq_count(&q), 3);          /* 3 个进程都在队列里 */
    CHECK_EQ(policy_readyq_pop(&q), 1);            /* 又回到进程 1 */

    UTEST_SUMMARY("test_sched");
}
