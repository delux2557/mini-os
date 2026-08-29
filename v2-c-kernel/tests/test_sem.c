/* mini-os/v2-c-kernel/tests/test_sem.c
 * 信号量宿主单元测试：只编译 src/sem.c（纯逻辑），
 * 验证计数增减、等待队列 FIFO 顺序、满队列边界、"应阻塞"标记等。
 */
#include "utest.h"
#include "sem.h"

int main(void) {
    sem_t s;
    uint32_t i;

    /* 1) init：计数正确、等待队列为空 */
    sem_init(&s, 2);
    CHECK_EQ(s.count, 2);
    CHECK_EQ(sem_wait_count(&s), 0);

    /* 2) 资源充足时 wait 直接占用：count 递减、不阻塞 */
    CHECK_EQ(sem_wait_try(&s, 1), 0);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(sem_wait_try(&s, 2), 0);
    CHECK_EQ(s.count, 0);
    CHECK_EQ(sem_wait_count(&s), 0);

    /* 3) 资源耗尽时 wait 返回"应阻塞"并入队 */
    CHECK_EQ(sem_wait_try(&s, 3), 1);
    CHECK_EQ(sem_wait_count(&s), 1);
    CHECK_EQ(s.waiters[0], 3);
    CHECK_EQ(s.count, 0);

    /* 4) signal 唤醒队首等待者：count 不变 */
    CHECK_EQ(sem_signal_wake(&s), 3);
    CHECK_EQ(s.count, 0);
    CHECK_EQ(sem_wait_count(&s), 0);

    /* 5) 无等待者时 signal：count++、返回 SEM_NO_PID */
    sem_init(&s, 0);
    CHECK_EQ(sem_signal_wake(&s), SEM_NO_PID);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(sem_signal_wake(&s), SEM_NO_PID);
    CHECK_EQ(s.count, 2);

    /* 6) 多个等待者按 FIFO 顺序唤醒 */
    sem_init(&s, 0);
    CHECK_EQ(sem_wait_try(&s, 10), 1);
    CHECK_EQ(sem_wait_try(&s, 11), 1);
    CHECK_EQ(sem_wait_try(&s, 12), 1);
    CHECK_EQ(sem_wait_count(&s), 3);
    CHECK_EQ(sem_signal_wake(&s), 10);
    CHECK_EQ(sem_signal_wake(&s), 11);
    CHECK_EQ(sem_signal_wake(&s), 12);
    CHECK_EQ(s.count, 0);
    CHECK_EQ(sem_wait_count(&s), 0);

    /* 7) 互斥锁语义：signal 唤醒等待者即把资源交给它（被唤醒者不再 wait）。
     *    A 拿锁 -> B/C 阻塞 -> A 释放唤醒 B -> B 释放唤醒 C -> C 释放归还资源 */
    sem_init(&s, 1);
    CHECK_EQ(sem_wait_try(&s, 1), 0);      /* A 拿锁 */
    CHECK_EQ(sem_wait_try(&s, 2), 1);      /* B 阻塞 */
    CHECK_EQ(s.count, 0);
    CHECK_EQ(sem_wait_try(&s, 3), 1);      /* C 也阻塞 */
    CHECK_EQ(sem_signal_wake(&s), 2);      /* A 释放 -> 唤醒 B（B 直接获得锁） */
    CHECK_EQ(s.count, 0);                  /* 锁在 B 手中，count 不变 */
    CHECK_EQ(sem_signal_wake(&s), 3);      /* B 释放 -> 唤醒 C */
    CHECK_EQ(s.count, 0);
    CHECK_EQ(sem_wait_count(&s), 0);       /* 等待队列清空 */
    CHECK_EQ(sem_signal_wake(&s), SEM_NO_PID);  /* C 释放 -> 无等待者 */
    CHECK_EQ(s.count, 1);                  /* 资源归还 */

    /* 8) 计数型信号量：初始 3，4 个进程，最后一个阻塞 */
    sem_init(&s, 3);
    for (i = 1; i <= 3; i++) CHECK_EQ(sem_wait_try(&s, i), 0);
    CHECK_EQ(sem_wait_try(&s, 4), 1);      /* 第 4 个等待 */
    CHECK_EQ(s.count, 0);
    CHECK_EQ(sem_signal_wake(&s), 4);      /* 任一释放即唤醒 */

    /* 9) 等待队列满：返回 -1（失败），不再入队 */
    sem_init(&s, 0);
    for (i = 0; i < SEM_MAX_WAITERS; i++) CHECK_EQ(sem_wait_try(&s, i + 1), 1);
    CHECK_EQ(sem_wait_count(&s), SEM_MAX_WAITERS);
    CHECK_EQ(sem_wait_try(&s, 99), -1);
    CHECK_EQ(sem_wait_count(&s), SEM_MAX_WAITERS);   /* 未再入队 */

    /* 10) 计数守恒：初始 2，两人占用后两人等待；依次唤醒后由持有者释放归还 */
    sem_init(&s, 2);
    CHECK_EQ(sem_wait_try(&s, 1), 0);      /* 占用 1 */
    CHECK_EQ(sem_wait_try(&s, 2), 0);      /* 占用 2，count=0 */
    CHECK_EQ(sem_wait_try(&s, 3), 1);      /* 入队 */
    CHECK_EQ(sem_wait_try(&s, 4), 1);      /* 入队 */
    CHECK_EQ(sem_signal_wake(&s), 3);      /* 唤醒 3（获得资源，count 不变） */
    CHECK_EQ(s.count, 0);
    CHECK_EQ(sem_signal_wake(&s), 4);      /* 唤醒 4 */
    CHECK_EQ(s.count, 0);
    CHECK_EQ(sem_wait_count(&s), 0);       /* 队列清空 */
    CHECK_EQ(sem_signal_wake(&s), SEM_NO_PID);  /* 3 释放 */
    CHECK_EQ(s.count, 1);
    CHECK_EQ(sem_signal_wake(&s), SEM_NO_PID);  /* 4 释放 */
    CHECK_EQ(s.count, 2);                  /* 资源守恒：回到初始 2 */

    /* 11) 不变量审计：正常序列恒成立，人为破坏能检出（v0.21） */
    sem_init(&s, 1);
    CHECK_EQ(sem_invariant_ok(&s), 1);
    CHECK_EQ(sem_wait_try(&s, 1), 0);      /* 占用：count=0 */
    CHECK_EQ(sem_invariant_ok(&s), 1);
    CHECK_EQ(sem_wait_try(&s, 2), 1);      /* 入队：count=0, waiters=1 */
    CHECK_EQ(sem_invariant_ok(&s), 1);
    CHECK_EQ(sem_signal_wake(&s), 2);      /* 唤醒：count=0, waiters=0 */
    CHECK_EQ(sem_invariant_ok(&s), 1);
    CHECK_EQ(sem_signal_wake(&s), SEM_NO_PID);  /* 归还：count=1 */
    CHECK_EQ(sem_invariant_ok(&s), 1);
    sem_init(&s, 1);
    s.wait_count = 1; s.waiters[0] = 7;    /* 人为破坏：有资源却排着队 */
    CHECK_EQ(sem_invariant_ok(&s), 0);
    sem_init(&s, 0);
    s.count = -1;                          /* 人为破坏：负计数 */
    CHECK_EQ(sem_invariant_ok(&s), 0);

    UTEST_SUMMARY("test_sem");
}
