/* mini-os/v2-c-kernel/src/apps/args.c
 * v0.12 argv 参数传递演示：
 *  - 内核以 cdecl 进入 app_main(int argc, char **argv)
 *  - argv[0]=程序名，argv[1..] 为 exec 传参
 *  - 打印 argc 与每个参数（配合 shell 的 exec 命令：exec args hello world）
 */
#include "user_lib.h"

void app_main(int argc, char **argv) {
    sys_print("[args] pid=");
    user_putdec(sys_getpid());
    sys_print(" argc=");
    user_putdec((uint32_t)argc);
    sys_print("\n");
    for (int i = 0; i < argc && i < 8; i++) {
        sys_print("[args] argv[");
        user_putdec((uint32_t)i);
        sys_print("]='");
        if (argv[i]) sys_print(argv[i]);
        sys_print("'\n");
    }
}
