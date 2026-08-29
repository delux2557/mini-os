/* mini-os/v2-c-kernel/src/apps/hello.c
 * 最简 ELF 应用：打印问候语与 pid 后退出（演示"从文件系统加载程序"）。 */
#include "user_lib.h"

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("Hello from 'hello' app! pid=");
    user_putdec(sys_getpid());
    sys_print(" ticks=");
    user_putdec(sys_getticks());
    sys_print("\n");
    sys_exit(0);
}
