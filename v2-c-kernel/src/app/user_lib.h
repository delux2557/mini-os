/* mini-os/v2-c-kernel/user_lib.h
 * 用户态共享库（v0.9）：系统调用号、syscall 内联封装、打印/字符串小工具。
 * 由 shell / hello / echo / crash 等独立编译的 ELF 应用 #include。
 * 与内核 usermode.c 的 syscall 分发表严格对应。
 */
#ifndef _USER_LIB_H
#define _USER_LIB_H
#include <stdint.h>
#include "netio.h"   /* v0.20: 网络 I/O 参数结构（与内核 ABI 一致） */

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
#define SYS_FS_SYNC    29   /* v0.16: 把 ramdisk 全量写回真盘（持久化） */
#define SYS_NET_SOCKET  30  /* v0.20: 创建 UDP socket（port=0 自动分配），返回 socket id */
#define SYS_NET_SENDTO  31  /* v0.20: 发 UDP 数据报（参数在 net_send_iov 中） */
#define SYS_NET_RECVFROM 32 /* v0.20: 非阻塞收 UDP 数据报（0=无包；出参在 net_recv_iov） */
#define SYS_NET_CLOSE   33  /* v0.20: 关闭 socket */
#define SYS_KERN_AUDIT  34  /* v0.21: 内核自审计（帧配平/信号量守恒/PCB 状态机） */
#define SYS_BRK         35  /* v0.26#2: 用户堆（brk/sbrk） */
#define SYS_LIMIT       36  /* v0.34 BUG-058: per-process syscall 掩码（只收窄） */

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
static inline int sys_fs_sync(void) { return (int)syscall3(SYS_FS_SYNC, 0, 0, 0); }

/* v0.20: 用户态 UDP socket（参数结构见 netio.h） */
static inline int sys_net_socket(uint16_t port) {
    return (int)syscall3(SYS_NET_SOCKET, port, 0, 0);
}
static inline int sys_net_sendto(int sock, const struct net_send_iov *iov) {
    return (int)syscall3(SYS_NET_SENDTO, (uint32_t)sock, (uint32_t)iov, 0);
}
static inline int sys_net_recvfrom(int sock, struct net_recv_iov *iov) {
    return (int)syscall3(SYS_NET_RECVFROM, (uint32_t)sock, (uint32_t)iov, 0);
}
static inline int sys_net_close(int sock) {
    return (int)syscall3(SYS_NET_CLOSE, (uint32_t)sock, 0, 0);
}
/* v0.21: 内核自审计——返回失败检查项总数（0=全部通过） */
static inline uint32_t sys_kern_audit(void) {
    return syscall3(SYS_KERN_AUDIT, 0, 0, 0);
}
/* v0.26#2: 用户堆。sys_brk(addr)：addr=0 查询当前 brk，否则设置 program break（0/-1）；
 * sys_sbrk(incr)：incr 字节增长，返回旧 brk（-1=失败，可作 sbrk(0) 查询） */
static inline int sys_brk(uint32_t addr) {
    return (int)syscall3(SYS_BRK, addr, 0, 0);
}
static inline uint32_t sys_sbrk(int32_t incr) {
    uint32_t old = sys_brk(0);
    if (incr == 0) return old;
    uint32_t nbrk = (uint32_t)((int64_t)old + (int64_t)incr);
    if (sys_brk(nbrk) == 0) return old;
    return (uint32_t)-1;
}

/* v0.34 BUG-058 per-process syscall 掩码（最小权限）：
 * sys_limit(mask_lo, mask_hi) 只收窄（内核与当前掩码 |），无放宽。bit i = 禁用 syscall i。
 * 掩码位以"开放面/封闭面"两类宏给出，dev 组合时勿误禁生存必需项：
 *   exit(0) / print(1) / getpid(5) / sleep(3) / yield(4) 等不在禁用建议内。 */
static inline uint32_t sys_limit(uint32_t mask_lo, uint32_t mask_hi) {
    return syscall3(SYS_LIMIT, mask_lo, mask_hi, 0);
}
#define SC_SEN   (1ull << 31)   /* net_sendto */
#define SC_RECV  (1ull << 32)   /* net_recvfrom */
#define SC_NETC  (1ull << 33)   /* net_close */
#define SC_NET   (SC_SEN | SC_RECV | SC_NETC)      /* 整条网络面 */
#define SC_FSC   (1ull << 13)   /* fs_create */
#define SC_FSO   (1ull << 14)   /* fs_open（写/追加口）*/
#define SC_FSW   (1ull << 15)   /* fs_write */
#define SC_FSR   (1ull << 16)   /* fs_read（dev 按需取舍，非必须禁）*/
#define SC_FSD   (1ull << 19)   /* fs_delete */
#define SC_FSM   (1ull << 27)   /* fs_mkdir */
#define SC_FSRD  (1ull << 28)   /* fs_rmdir */
#define SC_FSS   (1ull << 29)   /* fs_sync */
#define SC_FS_W  (SC_FSC | SC_FSO | SC_FSW | SC_FSD | SC_FSM | SC_FSRD | SC_FSS)
#define SC_LIMIT_LO(m) ((uint32_t)((m) & 0xFFFFFFFFull))
#define SC_LIMIT_HI(m) ((uint32_t)(((m) >> 32) & 0xFFFFFFFFull))

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
