/* mini-os/v2-c-kernel/src/apps/crt.c
 * 用户态 CRT 入口（v0.16 收口）：把 ELF entry 由 app_main 提升为 _start。
 * 内核以 cdecl 进入——栈上 [esp]=返回地址, [esp+4]=argc, [esp+8]=argv。
 * _start 取 argc/argv 调用 app_main，返回后统一 sys_exit(0)：
 * 根治"app_main 忘了 sys_exit 就从栈槽顶未映射处 ret"这类崩溃（BUG-016），
 * 各应用不再需要手写尾部 sys_exit(0)。
 */
#include "user_lib.h"

/* ---- v1.5 P2 编译硬化：用户态栈金丝雀运行时（教学增强）----
 * -fstack-protector-all 在 app 函数序言插入金丝雀，返回前校验；写爆时交给这里处理。
 * 两个符号必须由每个用户态 ELF 提供（经 crt.o 链入）。仅 host gcc 编的 app 生效；
 * guest 内 cc500 自编译程序是 C 子集、无此机制——属预期不对称。
 * 守卫固定值便于教学演示（对"防 AI 新增代码的意外越界"有效）；非防定向攻击——
 * 已知 0xdeadbeef 的针对性覆盖可绕过。若需真加固可改为每进程随机值（师生可扩展点）。
 */
uintptr_t __stack_chk_guard = 0xdeadbeef;

void __attribute__((noreturn)) __stack_chk_fail(void) {
    sys_print("[CRIT] stack smashing detected\n");
    sys_exit(1);
    for (;;);   /* gcc 依 noreturn 不再返回；此循环兜底防优化器告警 */
}

void _start(int argc, char **argv) {
    app_main(argc, argv);
    sys_exit(0);
}
