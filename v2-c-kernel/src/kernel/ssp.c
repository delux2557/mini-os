/* mini-os/v2-c-kernel/src/kernel/ssp.c
 * 加固 A-1 ①：内核栈金丝雀（SSP）运行时。
 *
 * 教学点：app 层（crt.c）用固定值 0xdeadbeef 仅供演示；内核侧不应照搬固定金丝雀
 * ——固定值可被针对性覆盖绕过。故内核 guard 在启动早期用 CPU 时间戳计数器（RDTSC）
 * 混入随机初值。compiler-rt 语义：
 *   - __stack_chk_guard：-fstack-protector-strong 为含"栈帧"函数序言读入、返回前校验；
 *   - __stack_chk_fail： 金丝雀被改写（典型=局部缓冲区越界/写穿栈帧）由 epilogue 跳入，
 *     打印 pid/eip/esp 后走既有停机路径——宁要可诊断的停机，不要静默的内存破坏。
 */
#include <stdint.h>
#include "serial.h"
#include "vga.h"

/* 初值固定，仅覆盖引导早期（kernel_main 序言读入）；进入 ssp_seed 后即随机化。
 * 32 位无 TLS，必须 -mstack-protector-guard=global 让 gcc 读全局而非 GS 段。 */
uintptr_t __stack_chk_guard = (uintptr_t)0xC0FFEE0Du;

/* 启动早期随机化：读 RDTSC 混合高低位，异或固定魔数，保证非 0 且非 -1。
 * no_stack_protector：对此函数禁用金丝雀，避免其自身序言先读旧 guard（改后同帧
 * epilogue 再读新值比对必然不匹配——必须先写、不校验）。 */
__attribute__((no_stack_protector))
void ssp_seed(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uintptr_t tsc = ((uintptr_t)hi << 16) ^ lo;
    __stack_chk_guard = (tsc ^ 0xA55A5A5Au) | 1u;
    if ((intptr_t)__stack_chk_guard == (intptr_t)-1) __stack_chk_guard ^= 0x13579BDFu;
    serial_puts("[ssp] kernel stack guard randomized\n");
}

/* 金丝雀校验失败（=内核栈帧被破坏）→ 打印现场后停机。
 * no_stack_protector：此处自身不得再生成 canary 校验（栈已可疑，防止自指递归崩溃）。
 * 与 kstack_check（L0 页底哨兵）互补：本函数捕获"返回前的改写"，L0 兜住"写穿栈底"。 */
void __attribute__((noreturn, no_stack_protector)) __stack_chk_fail(void) {
    extern uint32_t sched_current_pid(void);
    uint32_t esp, eip;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    /* 进入本函数时 [esp+4] 是调用方压入的返回地址 = 被改写帧的下一条指令 */
    eip = *(uint32_t *)(esp + 4);
    serial_printf("\n[PANIC] kernel stack canary mismatch (__stack_chk_fail) "
                  "pid=%u eip=%x esp=%x\n"
                  "  => 内核栈帧被写穿，无法安全返回，停机（SSP）\n",
                  sched_current_pid(), eip, esp);
    vga_printf("\n[PANIC] kernel canary mismatch pid=%u eip=%x\n",
               sched_current_pid(), eip);
    __asm__ volatile("cli; hlt");
    for (;;);   /* noreturn 兜底：不可达 */
}