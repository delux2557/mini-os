/* mini-os/v2-c-kernel/src/app/semhold.c
 * v0.38「挂起可达性」不变量（`[audit] ipc ok`）的**在环被检对象**。
 *
 * 为什么需要它：该审计行会打印 `checked N` —— 若 N 恒为 0，这行只证明"检查跑过"，**没证明**
 * "判据能在真实等待者上给出正确结论"。而仓库里此前没有任何"停在 IPC 等待上"的用例：
 * 开机的 sem/msg demo（userprog.c 的 user_sem_*）会跑完，`run` 又是同步 wait。
 *
 * 本程序在**无人会 signal** 的信号量上永久 `sem_wait` ⇒ 自己常驻该信号量的 waiters[]，
 * 由测试侧用 `bg semhold`（后台 spawn、不等待）拉起。于是本轮后续 selftest 的 checked ≥ 1，
 * 成为一个**非真空**的验证：
 *   · 判据把"已登记"误判为"不可达"       ⇒ `[audit] ipc FAIL` + `[selftest] audit≠0` 立刻红；
 *   · 登记路径被破坏（#188 那类簿记失配） ⇒ 同样立刻红；
 *   · 看门狗 kind=3 的真实分支也会每 16 心跳被走到一遍（**不应有任何 [WATCHDOG] 输出**）。
 *
 * 注意：本进程**不得**被唤醒、也不得退出——一旦离开 waiters[] 本用例即失去意义，故若真的
 * 被唤醒会打印 UNEXPECTED 并以 code=9 退出，便于测试侧识别。 */
#include "user_lib.h"

#define SEM_PARK_ID 5u   /* 约定槽：全系统没有任何程序会 signal 它 */

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int id = (int)syscall3(SYS_SEM_CREATE, SEM_PARK_ID, 0, 0);
    if (id < 0) {
        sys_print("[semhold] sem_create FAILED\n");
        sys_exit(1);
    }
    sys_print("[semhold] pid=");
    user_putdec(sys_getpid());
    sys_print(" parked on sem ");
    user_putdec(SEM_PARK_ID);
    sys_print(" (nobody signals; stays in waiters[])\n");
    (void)syscall3(SYS_SEM_WAIT, SEM_PARK_ID, 0, 0);
    sys_print("[semhold] UNEXPECTED wake-up (must never happen)\n");
    sys_exit(9);
}
