/* mini-os/v2-c-kernel/src/app/badinsn.c
 * SEC-01 回归：ring3 执行非法指令 ud2（#UD = CPU exception 6）。
 * 修复前：内核 isr_handler 对用户态非页错误异常一律 cli;hlt → 整机停机（DoS）。
 * 修复后：与 pf_handler 同构，用户态异常隔离杀进程，系统继续运行。
 *         本 app 仅作探针：执行 ud2 即应看到 "[user] CPU EXCEPTION #6 ... -> killed"，
 *         且后续 shell 命令仍正常工作（系统未冻结）。 */
#include "user_lib.h"

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[badinsn] pid=");
    user_putdec(sys_getpid());
    sys_print(" ud2 illegal instruction from ring3\n");
    __asm__ volatile("ud2");          /* SEC-01: user-mode #UD：修复前整机停机 */
    sys_print("[badinsn] ERROR: should never reach here!\n");
    for (;;);
}