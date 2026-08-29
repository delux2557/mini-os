/* mini-os/v2-c-kernel/user_lib.h
 * 用户态共享库（v0.9）：系统调用号、syscall 内联封装、打印/字符串小工具。
 * 由 shell / hello / echo / crash 等独立编译的 ELF 应用 #include。
 * 与内核 usermode.c 的 syscall 分发表严格对应。
 */
#ifndef _USER_LIB_H
#define _USER_LIB_H
#include <stdint.h>

/* ---- 系统调用号（与内核 usermode.c 一致） ---- */
#define SYS_EXIT       0
#define SYS_PRINT      1
#define SYS_GET_TICKS  2
#define SYS_SLEEP      3
#define SYS_YIELD      4
#define SYS_GET_PID    5
#define SYS_SEM_CREATE 6
#define SYS_SEM_WAIT   7
#define SYS_SEM_SIGNAL 8
#define SYS_SHMEM      9
#define SYS_MSG_CREATE 10
#define SYS_MSG_SEND   11
#define SYS_MSG_RECV   12
#define SYS_FS_CREATE  13
#define SYS_FS_OPEN    14
#define SYS_FS_WRITE   15
#define SYS_FS_READ    16
#define SYS_FS_CLOSE   17
#define SYS_FS_LS      18
#define SYS_FS_DELETE  19
#define SYS_READLINE   20
#define SYS_SPAWN_FILE 21
#define SYS_WAIT       22
#define SYS_MAP_PAGE   23   /* v0.11: 在当前进程地址空间映射一张私有页 */
#define SYS_FORK       24   /* v0.12: 复制当前进程（地址空间深拷贝，共享内存共享） */
#define SYS_EXEC       25   /* v0.12: 加载 ELF 替换当前进程（可传 argv） */
#define SYS_FS_SEEK    26   /* v0.14: 定位打开文件槽的读写位置 */
#define SYS_FS_MKDIR   27   /* v0.14: 建目录（父目录须存在） */
#define SYS_FS_RMDIR   28   /* v0.14: 删空目录 */

/* ---- syscall 内联封装（int 0x80） ---- */
static inline uint32_t syscall3(uint32_t n, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return ret;
}

static inline uint32_t sys_print(const char *s) { return syscall3(SYS_PRINT, (uint32_t)s, 0, 0); }
static inline uint32_t sys_getpid(void)         { return syscall3(SYS_GET_PID, 0, 0, 0); }
static inline uint32_t sys_getticks(void)       { return syscall3(SYS_GET_TICKS, 0, 0, 0); }
static inline uint32_t sys_sleep(uint32_t t)    { return syscall3(SYS_SLEEP, t, 0, 0); }
static inline uint32_t sys_yield(void)          { return syscall3(SYS_YIELD, 0, 0, 0); }
static inline uint32_t sys_exit(uint32_t code)  { return syscall3(SYS_EXIT, code, 0, 0); }
static inline int sys_readline(char *buf, uint32_t max) {
    return (int)syscall3(SYS_READLINE, (uint32_t)buf, max, 0);
}
static inline int sys_spawn_file(const char *name) {
    return (int)syscall3(SYS_SPAWN_FILE, (uint32_t)name, 0, 0);
}
static inline int sys_wait(uint32_t pid, int *status) {
    /* v0.15: 经典 wait/waitpid——pid=-1 等任意子进程；返回回收的子进程 pid（-1=无子进程），
     * 退出码写入 *status（NULL 则丢弃）。 */
    return (int)syscall3(SYS_WAIT, pid, (uint32_t)status, 0);
}
static inline uint32_t sys_map_page(uint32_t addr) {
    return syscall3(SYS_MAP_PAGE, addr, 0, 0);
}
static inline uint32_t sys_shmem(uint32_t slot) {
    return syscall3(SYS_SHMEM, slot, 0, 0);
}
/* v0.12: 进程复制与镜像替换 */
static inline uint32_t sys_fork(void) {
    return syscall3(SYS_FORK, 0, 0, 0);
}
static inline int sys_exec(const char *name, uint32_t argc, const char **argv) {
    return (int)syscall3(SYS_EXEC, (uint32_t)name, argc, (uint32_t)argv);
}

/* ---- 打印工具（十进制 / 十六进制） ---- */
static inline void user_putdec(uint32_t n) {
    char buf[12], tmp[12];
    int i = 0, j = 0;
    if (n == 0) buf[i++] = '0';
    while (n) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i) tmp[j++] = buf[--i];
    tmp[j] = 0;
    sys_print(tmp);
}

static inline void user_puthex(uint32_t n) {
    static const char *hex = "0123456789abcdef";
    char buf[12];
    int i = 0, started = 0;
    buf[i++] = '0'; buf[i++] = 'x';
    for (int s = 28; s >= 0; s -= 4) {
        int d = (int)((n >> s) & 0xF);
        if (d || started || s == 0) { started = 1; buf[i++] = hex[d]; }
    }
    buf[i] = 0;
    sys_print(buf);
}

/* ---- 字符串小工具 ---- */
static inline uint32_t user_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline int user_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int user_strncmp(const char *a, const char *b, uint32_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* 每个 app 的统一入口（Makefile 用 -e app_main 指定为 ELF entry）。
 * v0.12: 内核以 cdecl 方式进入——栈上 [esp]=返回地址, [esp+4]=argc, [esp+8]=argv，
 * 由 exec/run 提供 argv（argv[0] 为程序名）；无需参数的应用忽略即可。 */
void app_main(int argc, char **argv);

#endif
