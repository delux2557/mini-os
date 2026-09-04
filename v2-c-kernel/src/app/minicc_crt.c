/* mini-os/v2-c-kernel/src/app/minicc_crt.c
 * minicc 编译器专用 CRT + 运行时（V1）。
 *  - 不 include user_lib.h：其 static inline syscall3 会与本文件提供的外部
 *    syscall3（供 minicc.o 链接）重名冲突，故这里用裸 asm 实现。
 *  - _start：以 minicc_main() 返回值 sys_exit（普通 crt.o 会吞掉退出码，
 *    无法把"编译成功/失败"传给 shell，故独立一份，同 cc500_crt.c）。
 *  - 入口约定（内核 exec 布局）：[esp]=fake_ret [esp+4]=argc [esp+8]=argv，
 *    故 _start 按 (int argc, char **argv) 声明；minicc_main 按
 *    (char *argv, int argc) 声明（与 cc500 保持同款参数顺序）。
 */
int minicc_main(char *argv, int argc);
#include <stdint.h>

/* 与 minicc ELF 入口 stub 同语义：eax=n, ebx=a, ecx=b, edx=c, int $0x80 */
int syscall3(int n, int a, int b, int c) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return (int)ret;
}

void _start(int argc, char **argv) {
    int rc = minicc_main((char *)argv, argc);
    syscall3(0, (uint32_t)rc, 0, 0);   /* SYS_EXIT */
}
