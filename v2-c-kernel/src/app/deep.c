/* mini-os/v2-c-kernel/src/app/deep.c
 * v0.26 用户栈按需生长演示：
 *  - 每进程初始栈页仅 4KB（槽顶一页，槽底守卫页为硬底），总容量 28KB（7 页）
 *  - 本程序递归分配 1KB 局部数组使 ESP 逐页下探；命中守卫页时内核补映射
 *    （[stack] grow）并重试，程序继续运行
 *  - 12KB 递归 > 初始 4KB -> 触发 3 次按需生长后存活；深越界才 STACK OVERFLOW
 */
#include "user_lib.h"

/* noinline + volatile：确保每层真正在栈上分配 buf，不被优化掉。
 * optimize("O0")：防止 -O2 把尾递归改写成循环（栈帧被复用，栈占用 <4KB 不触发生长）。 */
__attribute__((noinline, optimize("O0"))) static void deep(int n) {
    volatile char buf[1024];
    if (n <= 0) return;
    buf[0] = (char)n;                 /* volatile 写 = 真实内存访问，帧必存在 */
    if (buf[0] == 0x7F) sys_print("[deep] never\n");
    deep(n - 1);
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[deep] pid=");
    user_putdec(sys_getpid());
    sys_print(" recursing 12*1KB on a 4KB start stack...\n");
    deep(12);
    sys_print("[deep] survived 12KB recursion via stack growth\n");
    sys_exit(0);
}
