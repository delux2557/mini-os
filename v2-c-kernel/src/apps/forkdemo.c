/* mini-os/v2-c-kernel/src/apps/forkdemo.c
 * v0.12 fork 进程模型演示：
 *  - sys_fork() 复制当前进程：父子从 fork 调用点分叉继续执行
 *    * 父进程 fork 返回子 pid；子进程返回 0
 *  - 地址空间深拷贝：父子把同一虚拟地址（sys_map_page 私有页）映射到
 *    "不同的物理页"，各自写入互不可见 -> 两个 ISOLATED OK
 */
#include "user_lib.h"

#define SLOT_ADDR 0x80060000u   /* 避开共享内存区(0x8002xxxx)/app 槽(0x8004xxxx) */

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t mypid = sys_getpid();
    sys_print("[fork] pid=");
    user_putdec(mypid);
    sys_print(" before fork\n");

    uint32_t child = sys_fork();
    mypid = sys_getpid();   /* fork 深拷贝地址空间，子进程的 pid 是新分配的 */

    if (child == 0) {
        sys_print("[fork] CHILD pid=");
        user_putdec(mypid);
        sys_print(" fork returned 0\n");
    } else {
        sys_print("[fork] PARENT pid=");
        user_putdec(mypid);
        sys_print(" fork returned child=");
        user_putdec(child);
        sys_print("\n");
    }

    /* 双方都映射同一虚拟地址 -> 深拷贝保证落到不同物理页（互不可见） */
    if (sys_map_page(SLOT_ADDR) == (uint32_t)-1) {
        sys_print("[fork] pid=");
        user_putdec(mypid);
        sys_print(" map FAILED\n");
        sys_exit(2);
    }
    volatile uint32_t *slot = (volatile uint32_t *)SLOT_ADDR;
    uint32_t mine = 1000 + mypid;
    *slot = mine;
    sys_print("[fork] pid=");
    user_putdec(mypid);
    sys_print(" wrote ");
    user_putdec(mine);
    sys_print("\n");

    sys_sleep(10);   /* 让对端也写同一虚拟地址（不同物理页） */

    uint32_t back = *slot;
    sys_print("[fork] pid=");
    user_putdec(mypid);
    sys_print(" read back ");
    user_putdec(back);
    if (back == mine) {
        sys_print(" ISOLATED OK\n");
        sys_exit(0);
    }
    sys_print(" ISOLATION BROKEN\n");
    sys_exit(1);
}
