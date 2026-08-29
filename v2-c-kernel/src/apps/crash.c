/* mini-os/v2-c-kernel/src/apps/crash.c
 * 内存保护演示：ring3 下直接写内核显存 0xB8000（内核页，无 user 位）
 * -> 触发页错误 -> 内核隔离终止该进程。 */
#include "user_lib.h"

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[crash] writing kernel memory 0xB8000...\n");
    *(volatile uint32_t *)0xB8000 = 0x12345678;   /* ring3 访问内核页 -> PF -> kill */
    sys_print("[crash] ERROR: should never reach here!\n");
    for (;;);
}
