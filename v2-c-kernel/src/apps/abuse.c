/* mini-os/v2-c-kernel/src/apps/abuse.c
 * v0.17 syscall 边界校验（copyin/copyout）演示：用内核低地址 / 回绕地址调用
 * 各类涉用户指针的系统调用，校验它们全部被内核拒绝（返回 -1），
 * 证明用户程序无法借 syscall 读写内核内存（防越权）。
 * 合法路径（如建/删一个文件）应正常工作，最后打印 [abuse] verify OK。
 */
#include "user_lib.h"

static uint32_t fail = 0;

static void report(const char *what, uint32_t rc) {
    sys_print("[abuse] ");
    sys_print(what);
    sys_print(" -> ");
    user_putdec(rc);
    sys_print(rc == (uint32_t)-1 ? " (rejected)\n" : " (ACCEPTED!)\n");
    if (rc != (uint32_t)-1) fail++;
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[abuse] syscall boundary check: kernel pointers must be rejected\n");

    /* 内核低地址 0x100000（kernel 区）应全部被拒 */
    report("print@0x100000",       syscall3(SYS_PRINT, 0x100000u, 0, 0));
    report("create@0x100000",      syscall3(SYS_FS_CREATE, 0x100000u, 0, 0));
    report("open@0x100000",        syscall3(SYS_FS_OPEN, 1, 0x100000u, 0));
    report("ls@0x100000",          syscall3(SYS_FS_LS, 0x100000u, 0, 0));
    report("delete@0x100000",      syscall3(SYS_FS_DELETE, 0x100000u, 0, 0));
    report("mkdir@0x100000",       syscall3(SYS_FS_MKDIR, 0x100000u, 0, 0));
    report("rmdir@0x100000",       syscall3(SYS_FS_RMDIR, 0x100000u, 0, 0));
    report("readline@0x100000",    syscall3(SYS_READLINE, 0x100000u, 64, 0));
    report("spawn@0x100000",       syscall3(SYS_SPAWN_FILE, 0x100000u, 0, 0));
    report("exec@0x100000",        syscall3(SYS_EXEC, 0x100000u, 0, 0));
    report("wait status@0x100000", syscall3(SYS_WAIT, (uint32_t)-1, 0x100000u, 0));
    /* 高地址回绕（> END，接近 4GB）也应被拒 */
    report("print@0xFFFFFFF0",     syscall3(SYS_PRINT, 0xFFFFFFF0u, 0, 0));

    /* 文件读写缓冲指针：先打开一个合法文件，再用内核地址当缓冲 -> 应被拒 */
    if (syscall3(SYS_FS_CREATE, (uint32_t)"/abuse.tmp", 0, 0) < 0) {
        sys_print("[abuse] create /abuse.tmp failed\n");
    } else if (syscall3(SYS_FS_OPEN, 1, (uint32_t)"/abuse.tmp", 1) != 0) {
        sys_print("[abuse] open /abuse.tmp failed\n");
    } else {
        report("write buf@0xB8000", syscall3(SYS_FS_WRITE, 1, 0xB8000u, 8));
        syscall3(SYS_FS_CLOSE, 1, 0, 0);
        syscall3(SYS_FS_DELETE, (uint32_t)"/abuse.tmp", 0, 0);
    }

    /* 合法路径应正常（不误伤）：建 -> 写 -> 删一个文件 */
    if (syscall3(SYS_FS_CREATE, (uint32_t)"/ok.tmp", 0, 0) < 0 ||
        syscall3(SYS_FS_OPEN, 1, (uint32_t)"/ok.tmp", 1) != 0) {
        sys_print("[abuse] valid-path FAIL\n");
        fail++;
    } else {
        const char msg[] = "abuse-ok";
        uint32_t w = syscall3(SYS_FS_WRITE, 1, (uint32_t)msg, (uint32_t)(sizeof(msg) - 1));
        syscall3(SYS_FS_CLOSE, 1, 0, 0);
        syscall3(SYS_FS_DELETE, (uint32_t)"/ok.tmp", 0, 0);
        sys_print("[abuse] valid path write -> ");
        user_putdec(w);
        sys_print("\n");
        if (w != sizeof(msg) - 1) fail++;
    }

    sys_print(fail == 0 ? "[abuse] verify OK\n" : "[abuse] verify FAIL\n");
    /* 返回后由 CRT 统一退出 */
}
