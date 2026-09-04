/* mini-os/v2-c-kernel/src/app/sandboxdemo.c
 * v0.34 BUG-058 per-process syscall 掩码（最小权限，seccomp 教学版）演示：
 *  受限前 fs 可建/删 -> sys_limit(SC_FS_W|SC_NET) 单向收窄 -> 受限后 fs 写面与整条
 *  网络面 syscall 全部 -1 -> 生存必需项（print/getpid/sleep/exit）仍正常 ->
 *  sys_limit(0,0) 无法清位（仍受限，ISA |= 无放宽）-> verify OK。
 */
#include "user_lib.h"

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fail = 0;
    sys_print("[sandboxdemo] mask demo start\n");

    /* 受限前：用户文件可建可删 */
    if ((int)syscall3(SYS_FS_CREATE, (uint32_t)"/sb0.tmp", 0, 0) < 0) { fail++; sys_print(" [s] pre-create FAIL\n"); }
    else syscall3(SYS_FS_DELETE, (uint32_t)"/sb0.tmp", 0, 0);

    /* 单向收窄：禁 fs 写面 + 整条网络面（64 位掩码经 lo/hi 两参传入） */
    uint64_t m = SC_FS_W | SC_NET;
    sys_limit(SC_LIMIT_LO(m), SC_LIMIT_HI(m));

    /* 受限后：fs 写面 / 网络 syscall 全部 -1 */
    if ((int)syscall3(SYS_FS_CREATE, (uint32_t)"/sb1.tmp", 0, 0) != -1) { fail++; sys_print(" [s] masked fs_create not -1\n"); }
    if ((int)syscall3(SYS_NET_SENDTO, 0xDEADBEEFu, 0, 0) != -1) { fail++; sys_print(" [s] masked net_sendto not -1\n"); }
    if ((int)syscall3(SYS_NET_RECVFROM, 0xDEADBEEFu, 0, 0) != -1) { fail++; sys_print(" [s] masked net_recvfrom not -1\n"); }
    if ((int)syscall3(SYS_NET_CLOSE, 0xDEADBEEFu, 0, 0) != -1) { fail++; sys_print(" [s] masked net_close not -1\n"); }

    /* 生存必需项不受影响 */
    sys_print("[sandboxdemo] still alive pid=");
    user_putdec(sys_getpid());
    sys_print("\n");
    sys_sleep(1);

    /* 放宽尝试被拒：sys_limit(0,0) 只 or（无法清位）；fs_create 仍 -1 */
    sys_limit(0, 0);
    if ((int)syscall3(SYS_FS_CREATE, (uint32_t)"/sb2.tmp", 0, 0) != -1) { fail++; sys_print(" [s] widen attempt NOT rejected\n"); }

    sys_print(fail == 0 ? "[sandboxdemo] verify OK\n" : "[sandboxdemo] verify FAIL\n");
}