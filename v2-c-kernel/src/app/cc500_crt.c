/* mini-os/v2-c-kernel/src/app/cc500_crt.c
 * v0.27: cc500 编译器专用 CRT + 运行时。
 *  - 不 include user_lib.h：其 static inline syscall3 会与本文件提供的
 *    外部 syscall3（供 cc500.o 链接）重名冲突，故这里用裸 asm 实现。
 *  - _start：以 cc500_main() 返回值 sys_exit（普通 crt.o 会吞掉退出码，
 *    无法把"编译成功/失败"传给 shell，故独立一份）。
 *  - v0.27b：cc500_main 按 (char *argv, int argc) 声明（见 cc500.c 注释），
 *    故此处以 cc500_main(argv, argc) 传入（argv 在前）。
 */
int cc500_main(char *argv, int argc);
#include <stdint.h>

/* 与 be_start 内联 stub 同语义：eax=n, ebx=a, ecx=b, edx=c, int $0x80 */
int syscall3(int n, int a, int b, int c) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return (int)ret;
}

void _start(int argc, char **argv) {
    int rc = cc500_main(argv, argc);
    syscall3(0, (uint32_t)rc, 0, 0);   /* SYS_EXIT */
}
