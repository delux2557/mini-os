/* mini-os/v2-c-kernel/src/apps/echo.c
 * 演示用户态阻塞式 I/O：用 sys_readline 读一行并回显。 */
#include "user_lib.h"

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[80];
    sys_print("[echo] type a line and press Enter\n");
    int n = sys_readline(line, 80);
    if (n < 0) n = 0;
    sys_print("[echo] got ");
    user_putdec((uint32_t)n);
    sys_print(" bytes: [");
    sys_print(line);
    sys_print("]\n");
}
