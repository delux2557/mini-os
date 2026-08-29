/* mini-os/v2-c-kernel/src/apps/stackovf.c
 * v0.13 用户栈守卫页 / 栈溢出检测演示：
 *  - 每个用户进程的栈区 8KB = [守卫页 4KB(未映射) | 栈页 4KB(映射)]
 *  - 程序故意往本进程栈槽的守卫页写入 -> 页错误
 *  - 内核 pf_handler 识别为 STACK OVERFLOW 并终止该进程（内存安全演示）
 */
#include "user_lib.h"

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[stackovf] pid=");
    user_putdec(sys_getpid());
    sys_print(" starting; will smash the guard page\n");

    /* 读当前栈指针；esp 位于本进程栈槽的"栈页"（槽内高半页） */
    uint32_t esp;
    __asm__ volatile ("mov %%esp, %0" : "=r"(esp));
    /* 本进程栈槽基址 = esp 向下对齐 8KB = 守卫页起点（未映射陷阱页） */
    uint32_t guard = esp & ~0x1FFFu;
    sys_print("[stackovf] writing guard page @");
    user_puthex(guard);
    sys_print("\n");

    *(volatile uint32_t *)guard = 0xDEADBEEF;   /* 写未映射页 -> STACK OVERFLOW -> kill */

    sys_print("[stackovf] ERROR: should never reach here!\n");
    for (;;);
}
