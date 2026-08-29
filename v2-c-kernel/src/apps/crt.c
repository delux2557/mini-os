/* mini-os/v2-c-kernel/src/apps/crt.c
 * 用户态 CRT 入口（v0.16 收口）：把 ELF entry 由 app_main 提升为 _start。
 * 内核以 cdecl 进入——栈上 [esp]=返回地址, [esp+4]=argc, [esp+8]=argv。
 * _start 取 argc/argv 调用 app_main，返回后统一 sys_exit(0)：
 * 根治"app_main 忘了 sys_exit 就从栈槽顶未映射处 ret"这类崩溃（BUG-016），
 * 各应用不再需要手写尾部 sys_exit(0)。
 */
#include "user_lib.h"

void _start(int argc, char **argv) {
    app_main(argc, argv);
    sys_exit(0);
}
