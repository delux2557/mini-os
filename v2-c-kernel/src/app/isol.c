/* mini-os/v2-c-kernel/src/apps/isol.c
 * v0.11 每进程地址空间 / 物理内存隔离演示。
 *
 * 核心断言：两个并发实例把"同一个虚拟地址"经 sys_map_page 映射，
 * 在启用每进程地址空间时，二者落到"不同的物理页"，互不可见：
 *   - 各自写入自己的值 -> 睡眠交错 -> 读回仍是自己写的值（ISOLATED OK）
 * 若退化为共享页表（旧行为），后写者会覆盖同一物理页，读回即错（ISOLATION BROKEN）。
 *
 * 第一个实例（由 shell `run isol` 启动）通过共享内存槽 1 记旗标，
 * 生成第二个实例（同一 ELF，独立地址空间），从而实现并发隔离演示。 */
#include "user_lib.h"

#define SLOT_ADDR   0x80050000u   /* 两个实例共用的固定虚拟地址 */
#define FLAG_SLOT   1             /* 共享内存槽 1：0=尚未生成孪生实例 */

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t pid = sys_getpid();

    /* 只有第一个实例负责生成孪生实例（经共享内存旗标去重，避免递归无限生成） */
    volatile uint32_t *flag = (volatile uint32_t *)sys_shmem(FLAG_SLOT);
    if (*flag == 0) {
        *flag = 1;
        int twin = sys_spawn_file("isol");
        sys_print("[isol] pid=");
        user_putdec(pid);
        sys_print(" spawned twin pid=");
        user_putdec((uint32_t)twin);
        sys_print("\n");
    }

    /* 映射固定虚拟地址 -> 本进程私有物理页（清零） */
    if (sys_map_page(SLOT_ADDR) == (uint32_t)-1) {
        sys_print("[isol] pid=");
        user_putdec(pid);
        sys_print(" map FAILED\n");
        sys_exit(2);
    }
    sys_print("[isol] pid=");
    user_putdec(pid);
    sys_print(" map ok addr=");
    user_puthex(SLOT_ADDR);
    sys_print("\n");

    /* 写入自己独有的值，睡眠让孪生实例也写同一虚拟地址（不同物理页） */
    volatile uint32_t *slot = (volatile uint32_t *)SLOT_ADDR;
    uint32_t mine = 1000 + pid;
    *slot = mine;
    sys_print("[isol] pid=");
    user_putdec(pid);
    sys_print(" wrote ");
    user_putdec(mine);
    sys_print("\n");

    sys_sleep(10);

    /* 读回：若每进程地址空间生效，仍是自己写的值 */
    uint32_t back = *slot;
    sys_print("[isol] pid=");
    user_putdec(pid);
    sys_print(" read back ");
    user_putdec(back);
    if (back == mine) {
        sys_print(" ISOLATED OK\n");
        sys_exit(0);
    }
    sys_print(" ISOLATION BROKEN\n");
    sys_exit(1);
}
