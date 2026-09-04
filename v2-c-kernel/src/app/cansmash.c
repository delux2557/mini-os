/* mini-os/v2-c-kernel/src/app/cansmash.c
 * v1.5 P2 编译硬化：栈金丝雀（stack canary）触发演示。
 *  - 与 stackovf（守卫页越深越界杀进程）互补：这里演示"函数内局部缓冲区越界写"，
 *    在函数返回时被 gcc -fstack-protector-all 的金丝雀校验拦截。
 *  - 写越 256 字节（远超 char buf[16]），盖掉本函数栈帧上的金丝雀与返回地址区；
 *    随后 return 使 app_main 的 epilogue 做金丝雀校验，失败 -> __stack_chk_fail
 *    -> 打印 [CRIT] -> sys_exit(1)。注意金丝雀校验先于"恢复/返回"执行，故即便返回
 *    地址也被污染，仍由 __stack_chk_fail 收束，进程干净退出（code=1）。
 *  - 只写栈内（buf[0..255] 远未到守卫页），故走 canary 路径而非守卫页。
 *    教学点：canary 拦在"返回前"，比守卫页更早、更精确；两者都是"进程被杀、内核存活"。
 */
#include "user_lib.h"

/* volatile + 落盘：阻止 -O2 把越界写与 g_sink 消费整段优化掉 */
static volatile uint32_t g_sink;

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[cansmash] starting stack smash\n");

    char buf[16];
    int i;
    volatile char *p = buf;      /* volatile：让 gcc 不做循环 UB 分析（避免 -Waggressive-loop-optimizations），
                                  * 运行期越界仍真实发生；且防 -O2 整段删写 */
    /* 越界 64 字节：盖住本函数栈帧上的 canary 槽（esp+28）与保存的返回地址（esp+44），
     * 且循环能跑完不冲出栈顶。绝不能写满 256——用户栈顶距 esp 仅 ~100B，写多会先撞
     * 守卫页（page fault 在 canary 检查前杀进程，就走成 stackovf 而非 canary 路径了）。 */
    for (i = 0; i < 64; i++) {
        p[i] = (char)i;          /* 越界写：盖掉金丝雀 + 保存的返回地址 */
        g_sink = (uint32_t)p[i];
    }

    return;   /* 返回时 app_main 的 canary 校验失败 -> __stack_chk_fail（关键：必须 return 才会触发） */
}