/* mini-os/v2-c-kernel/src/app/deepexec.c
 * v0.29 回归盲区补格：已生长栈 × exec 组合。
 *  - 递归 12KB 使用户栈按需生长（[stack] grow）后，在最深帧处 sys_exec("hello")
 *  - sched_exec 释放已生长栈全部页（stack_frames）与旧地址空间，新建 4KB 全新栈
 *  - hello 在新栈上运行退出 0 -> 证明 exec-from-grown-stack 无泄漏/无崩溃
 * 覆盖：exec 路径释放多页生长栈 + 深调用点发起 exec 的替换/回收。
 */
#include "user_lib.h"

/* noinline + optimize("O0")：确保每层在栈上分配 buf、不尾递归优化（同 deep.c） */
__attribute__((noinline, optimize("O0"))) static void deep_exec(int n) {
    volatile char buf[1024];
    buf[0] = (char)n;
    if (n > 0) {
        if (buf[0] == 0x7F) sys_print("[deepexec] never\n");
        deep_exec(n - 1);
        return;
    }
    /* 最深帧（n==0）：此刻栈已按需生长 ~12KB */
    sys_print("[deepexec] pid=");
    user_putdec(sys_getpid());
    sys_print(" stack grown, exec'ing hello from depth\n");
    int rc = sys_exec("hello", 0, 0);
    /* 成功路径不返回；失败才到这 */
    sys_print("[deepexec] exec FAILED rc=");
    user_putdec((uint32_t)rc);
    sys_print("\n");
    sys_exit(1);
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[deepexec] pid=");
    user_putdec(sys_getpid());
    sys_print(" starting grown-stack x exec combo\n");
    deep_exec(12);
    sys_exit(0);
}
