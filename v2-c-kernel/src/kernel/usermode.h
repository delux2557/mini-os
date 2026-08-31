/* mini-os/v2-c-kernel/usermode.h
 * 用户态支持：
 *  - 重建 GDT（含 ring3 代码/数据段与 TSS 段）
 *  - 系统调用分发（int 0x80）
 *  - TSS.esp0 维护（调度器每切换进程更新） */
#ifndef _USERMODE_H
#define _USERMODE_H
#include <stdint.h>
#include "idt.h"   /* registers_t */

/* 段选择子（GDT 槽位） */
#define SEL_KCODE   0x08   /* 内核代码 DPL0 */
#define SEL_KDATA   0x10   /* 内核数据 DPL0 */
#define SEL_UCODE   0x18   /* 用户代码 DPL3（选择子即 0x1B） */
#define SEL_UDATA   0x20   /* 用户数据 DPL3（选择子即 0x23） */
#define SEL_UCODE_R3 0x1B
#define SEL_UDATA_R3 0x23
#define SEL_TSS     0x28

/* v0.9: 常驻 shell 的固定链接/加载地址（v0.26 迁址：栈区 0x80010000..0x80090000 之后） */
#define SHELL_LINK  0x80090000u
/* v0.9: 普通应用（hello/echo/...）的固定链接/加载地址（app 槽，退出即回收） */
#define APP_LINK    0x800A0000u

void usermode_init(void);
/* 系统调用分发（int 0x80 门进入；可能因退出/睡眠/让出而不返回） */
void syscall_dispatch(registers_t *r);
/* 更新 TSS.esp0：调度器切换进程时必须调用 */
void usermode_set_esp0(uint32_t esp0);
/* v0.9: 从文件系统加载 ELF 并创建用户进程（内核启动时加载 shell 也用此接口）。
 *  - vbase：链接/加载的固定虚拟基址；resident=1 常驻（帧不随退出回收，如 shell），
 *    0 走 app 槽（退出即回收，槽忙会拒绝）。name 为可读字符串指针（内核/用户均可）。
 *  - 返回 pid 或 -1。 */
int usermode_spawn_elf(const char *name, uint32_t vbase, int resident);

#endif
