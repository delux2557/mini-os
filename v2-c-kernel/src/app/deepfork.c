/* mini-os/v2-c-kernel/src/app/deepfork.c
 * v0.29 回归盲区补格：已生长栈 × fork 组合。
 *  - 父进程递归 12KB 使用户栈按需生长（[stack] grow），最深帧处 sys_fork()
 *  - 子进程继承"已生长栈"（sched_fork 深拷贝全部栈页）：校验继承栈帧内容一致，
 *    并在继承栈上继续递归 8KB（栈下探越过继承区，触发子进程新的 [stack] grow）
 *  - 父进程 sys_wait 回收子进程并校验退出码
 * 覆盖：fork 深拷贝已生长栈页 + 子进程 stack_fcount=0 后继续生长的记账/回收路径。
 */
#include "user_lib.h"

/* noinline + optimize("O0")：确保每层在栈上分配 buf、不尾递归优化（同 deep.c） */
__attribute__((noinline, optimize("O0"))) static void grow(int n, uint32_t mark) {
    volatile char buf[1024];
    buf[0] = (char)(n + mark);
    if (n > 0) {
        if (buf[0] == 0x7F) sys_print("[deepfork] never\n");
        grow(n - 1, mark);
        return;
    }
    sys_print("[deepfork] CHILD pid=");
    user_putdec(sys_getpid());
    sys_print(" grew beyond inherited stack, survived\n");
}

__attribute__((noinline, optimize("O0"))) static void deep(int n, uint32_t mark) {
    volatile char buf[1024];
    buf[0] = (char)(n + mark);
    if (n > 0) {
        if (buf[0] == 0x7F) sys_print("[deepfork] never\n");
        deep(n - 1, mark);
        return;
    }
    /* 最深帧（n==0）：此刻栈已按需生长 ~12KB */
    sys_print("[deepfork] pid=");
    user_putdec(sys_getpid());
    sys_print(" stack grown ~12KB, forking\n");
    uint32_t child = sys_fork();
    uint32_t mypid = sys_getpid();
    if (child == 0) {
        /* 继承的栈帧内容应与 fork 时一致（深拷贝校验） */
        if (buf[0] != (char)mark) {
            sys_print("[deepfork] CHILD INHERIT BROKEN\n");
            sys_exit(2);
        }
        sys_print("[deepfork] CHILD pid=");
        user_putdec(mypid);
        sys_print(" inherited grown stack (mark ok), recursing deeper\n");
        grow(8, mark);          /* 继承栈上再下探 8KB（合计 ~20KB < 28KB 槽上限） */
        sys_exit(0);
    }
    int st = -1;
    uint32_t wp = sys_wait(child, &st);
    sys_print("[deepfork] PARENT pid=");
    user_putdec(mypid);
    sys_print(" waited child=");
    user_putdec(wp);
    sys_print(" code=");
    user_putdec((uint32_t)st);
    sys_print("\n");
    if (wp == child && st == 0) {
        sys_print("[deepfork] fork-of-grown-stack OK\n");
        sys_exit(0);
    }
    sys_print("[deepfork] FAIL\n");
    sys_exit(1);
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[deepfork] pid=");
    user_putdec(sys_getpid());
    sys_print(" starting grown-stack x fork combo\n");
    deep(12, 0x41);
    sys_exit(0);
}
