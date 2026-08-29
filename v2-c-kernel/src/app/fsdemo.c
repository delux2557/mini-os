/* mini-os/v2-c-kernel/src/apps/fsdemo.c
 * v0.14 文件系统增强演示（目录层级 / 追加写 / seek / 间接块）：
 *  1) mkdir /etc、mkdir /etc/sub，把文件建进子目录
 *  2) 追加写：open(mode=2) 把两段配置追加到 /etc/conf.txt
 *  3) seek + 读回：定位到偏移 5 校验 "8080"
 *  4) 间接块：写 100000 字节大文件（跨入间接块），抽查 4 处偏移
 *  5) rmdir 拒绝非空目录；清空后逐级删除
 * 输出约定：每行用一个 sys_print 一次打印（原子行），避免被抢占时
 * 其它进程的周期输出拆断本行（便于回归按整行关键字校验）。
 */
#include "user_lib.h"

/* 把 tag + path + " -> " + 十进制值 拼成一行后单次打印 */
static void fs_report(const char *tag, const char *p, uint32_t v) {
    char buf[96];
    uint32_t i = 0;
    const char *t = tag;
    while (*t && i < 92) buf[i++] = *t++;
    if (p) while (*p && i < 92) buf[i++] = *p++;
    const char *s = " -> ";
    while (*s && i < 92) buf[i++] = *s++;
    char tmp[12];
    int j = 0;
    if (v == 0) tmp[j++] = '0';
    while (v) { tmp[j++] = (char)('0' + (v % 10)); v /= 10; }
    while (j && i < 92) buf[i++] = tmp[--j];
    buf[i] = '\n';
    buf[i + 1] = 0;
    sys_print(buf);
}

static void fs_mkdir_p(const char *p) {
    fs_report("[fsdemo] mkdir ", p, (uint32_t)(int)syscall3(SYS_FS_MKDIR, (uint32_t)p, 0, 0));
}

static void fs_create_p(const char *p) {
    fs_report("[fsdemo] create ", p, (uint32_t)(int)syscall3(SYS_FS_CREATE, (uint32_t)p, 0, 0));
}

/* 以 mode（1=写 2=追加）打开并写一段后关闭 */
static void fs_write_p(const char *p, const char *s, uint32_t mode) {
    if ((int)syscall3(SYS_FS_OPEN, 1, (uint32_t)p, mode) != 0) {
        fs_report("[fsdemo] open fail ", p, 0);
        return;
    }
    int n = (int)syscall3(SYS_FS_WRITE, 1, (uint32_t)s, (uint32_t)user_strlen(s));
    fs_report("[fsdemo] write '", s, (uint32_t)n);
    syscall3(SYS_FS_CLOSE, 1, 0, 0);
}

static void fs_del_p(const char *p) {
    fs_report("[fsdemo] delete ", p, (uint32_t)(int)syscall3(SYS_FS_DELETE, (uint32_t)p, 0, 0));
}

static void fs_rmdir_p(const char *p) {
    fs_report("[fsdemo] rmdir ", p, (uint32_t)(int)syscall3(SYS_FS_RMDIR, (uint32_t)p, 0, 0));
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_print("[fsdemo] v0.14 filesystem: dirs + append + seek + indirect\n");

    /* 1) 目录层级 */
    fs_mkdir_p("/etc");
    fs_mkdir_p("/etc/sub");
    fs_create_p("/etc/conf.txt");
    fs_create_p("/etc/sub/notes.txt");

    /* 2) 追加写（mode=2：每次从文件末尾开始） */
    fs_write_p("/etc/conf.txt", "port=8080\n", 2);
    fs_write_p("/etc/conf.txt", "host=0.0.0.0\n", 2);
    fs_write_p("/etc/sub/notes.txt", "nested file\n", 2);

    /* 根目录 ls：展示目录类型标记（etc/） */
    sys_print("[fsdemo] ls / (dirs marked '/'):\n");
    syscall3(SYS_FS_LS, (uint32_t)"/", 0, 0);

    /* 3) seek + 读回校验：conf.txt = "port=8080\nhost=0.0.0.0\n"，偏移 5 起是 "8080" */
    if ((int)syscall3(SYS_FS_OPEN, 1, (uint32_t)"/etc/conf.txt", 0) != 0) {
        sys_print("[fsdemo] open conf fail\n");
    } else {
        syscall3(SYS_FS_SEEK, 1, 5, 0);
        char rb[32];
        uint32_t rn = syscall3(SYS_FS_READ, 1, (uint32_t)rb, 17);   /* 恰好 "8080\nhost=0.0.0.0"（17B） */
        rb[rn] = 0;
        char okbuf[64];   /* 足够容纳前缀 + 读回 + "' -> OK\n"，不被截断 */
        uint32_t i = 0;
        const char *pfx = "[fsdemo] seek(5) read '";
        while (*pfx && i < 62) okbuf[i++] = *pfx++;
        const char *c = rb;
        while (*c && i < 62) okbuf[i++] = *c++;
        const char *suf = user_strncmp(rb, "8080", 4) == 0 ? "' -> OK\n" : "' -> FAIL\n";
        while (*suf && i < 62) okbuf[i++] = *suf++;
        okbuf[i] = 0;
        sys_print(okbuf);
        syscall3(SYS_FS_CLOSE, 1, 0, 0);
    }

    /* 4) 间接块大文件：100000 字节 > 12 直接块(48KB)，跨入间接块 */
    fs_create_p("/big.bin");
    if ((int)syscall3(SYS_FS_OPEN, 1, (uint32_t)"/big.bin", 1) != 0) {
        sys_print("[fsdemo] open big fail\n");
    } else {
        char chunk[128];
        uint32_t total = 100000;
        uint32_t off = 0;
        while (off < total) {
            uint32_t n = total - off; if (n > 128) n = 128;
            for (uint32_t k = 0; k < n; k++) chunk[k] = (char)((off + k) & 0xFF);
            uint32_t w = syscall3(SYS_FS_WRITE, 1, (uint32_t)chunk, n);
            if (w != n) { sys_print("[fsdemo] big write short\n"); break; }
            off += n;
        }
        syscall3(SYS_FS_CLOSE, 1, 0, 0);

        /* 读回抽查：偏移 0 / 直接块末 / 间接块首 / 文件末 */
        uint32_t spots[4];
        spots[0] = 0; spots[1] = 12 * 4096 - 1; spots[2] = 12 * 4096; spots[3] = total - 1;
        int ok = 1;
        if ((int)syscall3(SYS_FS_OPEN, 1, (uint32_t)"/big.bin", 0) != 0) {
            sys_print("[fsdemo] reopen big fail\n");
            ok = 0;
        } else {
            for (int s = 0; s < 4; s++) {
                syscall3(SYS_FS_SEEK, 1, spots[s], 0);
                char ch = 0;
                uint32_t rn2 = syscall3(SYS_FS_READ, 1, (uint32_t)&ch, 1);
                char exp = (char)(spots[s] & 0xFF);
                if (rn2 != 1 || ch != exp) {
                    sys_print("[fsdemo] big spot FAIL\n");
                    ok = 0;
                    break;
                }
            }
            syscall3(SYS_FS_CLOSE, 1, 0, 0);
        }
        sys_print(ok ? "[fsdemo] big.bin 100000B indirect spot-check OK\n"
                     : "[fsdemo] big.bin 100000B indirect spot-check FAIL\n");
    }

    /* 5) rmdir：非空目录拒绝；清空后逐级删除 */
    sys_print("[fsdemo] ls /etc:\n");
    syscall3(SYS_FS_LS, (uint32_t)"/etc", 0, 0);
    fs_rmdir_p("/etc");                 /* 非空 -> -1 */
    fs_rmdir_p("/etc/sub");             /* 非空 -> -1 */
    fs_del_p("/etc/conf.txt");
    fs_del_p("/etc/sub/notes.txt");
    fs_del_p("/big.bin");
    fs_mkdir_p("/tmp");
    fs_rmdir_p("/etc/sub");
    fs_rmdir_p("/etc");
    fs_rmdir_p("/tmp");

    sys_print("[fsdemo] done\n");
    /* 收口（v0.16）：app_main 返回后由 CRT 的 _start 统一 sys_exit(0) */
}
