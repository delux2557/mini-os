/* mini-os/v2-c-kernel/userprog.c
 * 用户程序集（v0.6 新增 IPC/同步演示）：
 *  - 独立编译、链接到 0x80000000，以 flat binary 内嵌进内核
 *  - 运行在 CPU ring 3，只能通过 int 0x80 系统调用请求内核服务
 *  - procA/procB：抢占调度演示（忙等 + sleep）
 *  - procSemA/procSemB：信号量演示
 *      * rendezvous 会合（两个信号量双向等待，展示阻塞/唤醒）
 *      * 互斥锁保护共享内存中的计数器（持锁 sleep，强制对端在 wait 上阻塞）
 *  - procCrash：越权访问被内核隔离终止
 */
#include <stdint.h>

#define SYS_EXIT     0
#define SYS_PRINT    1
#define SYS_GET_TICKS 2
#define SYS_SLEEP    3
#define SYS_YIELD    4
#define SYS_GET_PID  5
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

/* 信号量 id 约定（与内核 sem 槽对应） */
#define SEM_MUTEX  1   /* 互斥锁：保护共享计数 */
#define SEM_A2B    2   /* A -> B：A 已到达 */
#define SEM_B2A    3   /* B -> A：B 已到达 */

/* 消息队列 id 约定（与内核 msg 槽对应） */
#define MSG_PIPE   1   /* 生产者 -> 消费者的有界管道（容量 4） */

static inline uint32_t syscall3(uint32_t n, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return ret;
}

/* 打印字符串：系统调用以 uint32_t 承载指针，需显式窄化转换，
 * 否则 GCC 14 默认把隐式 int-conversion 升级为编译错误。 */
static inline void sys_print(const char *s) {
    syscall3(SYS_PRINT, (uint32_t)s, 0, 0);
}

/* 简单十进制转字符串并打印 */
static void putdec(uint32_t n) {
    char buf[12], tmp[12];
    int i = 0, j = 0;
    if (n == 0) buf[i++] = '0';
    while (n) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i) tmp[j++] = buf[--i];
    tmp[j] = 0;
    syscall3(SYS_PRINT, (uint32_t)tmp, 0, 0);
}

/* 十六进制打印（地址用） */
static void puthex(uint32_t n) {
    static const char *hex = "0123456789abcdef";
    char buf[12];
    int i = 0, started = 0;
    buf[i++] = '0'; buf[i++] = 'x';
    for (int s = 28; s >= 0; s -= 4) {
        int d = (int)((n >> s) & 0xF);
        if (d || started || s == 0) { started = 1; buf[i++] = hex[d]; }
    }
    buf[i] = 0;
    syscall3(SYS_PRINT, (uint32_t)buf, 0, 0);
}

/* 忙等 n 个心跳。期间定时器会抢占本进程让其它进程运行 */
static void spin_ticks(uint32_t n) {
    uint32_t start = syscall3(SYS_GET_TICKS, 0, 0, 0);
    while (syscall3(SYS_GET_TICKS, 0, 0, 0) - start < n) { }
}

/* 进程 A：放在 binary 最前（offset 0） */
__attribute__((section(".text.entry")))
void user_main_a(void) {
    sys_print("[A] procA started, pid=");
    putdec(syscall3(SYS_GET_PID, 0, 0, 0));
    sys_print("\n");
    for (;;) {
        spin_ticks(25);
        sys_print("[A] tick=");
        putdec(syscall3(SYS_GET_TICKS, 0, 0, 0));
        sys_print("\n");
        syscall3(SYS_SLEEP, 10, 0, 0);
    }
}

/* 进程 B */
__attribute__((section(".text.b")))
void user_main_b(void) {
    sys_print("[B] procB started, pid=");
    putdec(syscall3(SYS_GET_PID, 0, 0, 0));
    sys_print("\n");
    for (;;) {
        spin_ticks(15);
        sys_print("[B] tick=");
        putdec(syscall3(SYS_GET_TICKS, 0, 0, 0));
        sys_print("\n");
        syscall3(SYS_SLEEP, 10, 0, 0);
    }
}

/* ---- v0.6 信号量演示 ---- */

/* procSemA：rendezvous + 互斥共享计数 */
__attribute__((section(".text.sem")))
void user_sem_a(void) {
    int mutex = (int)syscall3(SYS_SEM_CREATE, SEM_MUTEX, 1, 0);
    int a2b   = (int)syscall3(SYS_SEM_CREATE, SEM_A2B,   0, 0);
    int b2a   = (int)syscall3(SYS_SEM_CREATE, SEM_B2A,   0, 0);
    uint32_t sh = syscall3(SYS_SHMEM, 0, 0, 0);
    uint32_t *cnt = (uint32_t *)sh;

    sys_print("[SA] sem demo pid=");
    putdec(syscall3(SYS_GET_PID, 0, 0, 0));
    sys_print(" shmem=");
    puthex(sh);
    sys_print("\n");

    /* rendezvous：通知 B 已到，然后等 B 到达（双向会合） */
    sys_print("[SA] arrived, rendezvous...\n");
    syscall3(SYS_SEM_SIGNAL, a2b, 0, 0);
    syscall3(SYS_SEM_WAIT,   b2a, 0, 0);
    sys_print("[SA] rendezvous done\n");

    /* 互斥共享计数：持锁后 sleep，强制对端在 wait 上阻塞并等待唤醒 */
    for (int i = 0; i < 5; i++) {
        syscall3(SYS_SEM_WAIT, mutex, 0, 0);
        (*cnt)++;
        sys_print("[SA] locked cnt=");
        putdec(*cnt);
        sys_print("\n");
        syscall3(SYS_SLEEP, 5, 0, 0);      /* 持锁睡觉 -> 对端 sem_wait 阻塞 */
        syscall3(SYS_SEM_SIGNAL, mutex, 0, 0);
        syscall3(SYS_SLEEP, 3, 0, 0);
    }
    sys_print("[SA] done\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}

/* procSemB：与 A 对称 */
__attribute__((section(".text.sem")))
void user_sem_b(void) {
    int mutex = (int)syscall3(SYS_SEM_CREATE, SEM_MUTEX, 1, 0);
    int a2b   = (int)syscall3(SYS_SEM_CREATE, SEM_A2B,   0, 0);
    int b2a   = (int)syscall3(SYS_SEM_CREATE, SEM_B2A,   0, 0);
    uint32_t sh = syscall3(SYS_SHMEM, 0, 0, 0);
    uint32_t *cnt = (uint32_t *)sh;

    sys_print("[SB] sem demo pid=");
    putdec(syscall3(SYS_GET_PID, 0, 0, 0));
    sys_print("\n");

    sys_print("[SB] arrived, rendezvous...\n");
    syscall3(SYS_SEM_SIGNAL, b2a, 0, 0);
    syscall3(SYS_SEM_WAIT,   a2b, 0, 0);
    sys_print("[SB] rendezvous done\n");

    for (int i = 0; i < 5; i++) {
        syscall3(SYS_SEM_WAIT, mutex, 0, 0);
        (*cnt)++;
        sys_print("[SB] locked cnt=");
        putdec(*cnt);
        sys_print("\n");
        syscall3(SYS_SLEEP, 5, 0, 0);
        syscall3(SYS_SEM_SIGNAL, mutex, 0, 0);
        syscall3(SYS_SLEEP, 3, 0, 0);
    }
    sys_print("[SB] done\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}

/* 崩溃演示：尝试直接写内核显存 0xB8000（内核页, 无 user 位）-> 页错误 -> 被杀 */
__attribute__((section(".crash")))
void user_crash(void) {
    sys_print("[C] crash demo: writing kernel memory 0xB8000...\n");
    *(volatile uint32_t *)0xB8000 = 0x12345678;   /* ring3 访问内核页 -> PF */
    sys_print("[C] ERROR: should never reach here!\n");
    for (;;);
}

/* ---- v0.7 消息队列演示：生产者-消费者（有界缓冲 + 阻塞/唤醒） ---- */
#define MSG_N 20   /* 收发条数 */

/* procMsgC：消费者。先被创建并运行，立即在空缓冲上 recv 阻塞（演示 recv-block），
 * 之后每次收一条慢消费，迫使生产者赶上并塞满缓冲而阻塞（演示 send-block）。 */
__attribute__((section(".text.msg")))
void user_msg_c(void) {
    syscall3(SYS_MSG_CREATE, MSG_PIPE, 4, 0);
    sys_print("[MC] consumer started, pid=");
    putdec(syscall3(SYS_GET_PID, 0, 0, 0));
    sys_print("\n");
    for (int i = 0; i < MSG_N; i++) {
        uint32_t v = syscall3(SYS_MSG_RECV, MSG_PIPE, 0, 0);
        sys_print("[MC] got val=");
        putdec(v);
        sys_print("\n");
        syscall3(SYS_SLEEP, 3, 0, 0);   /* 慢消费：3 tick */
    }
    sys_print("[MC] done\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}

/* procMsgP：生产者。快速发送 0..19，缓冲满时阻塞，由消费者消费后唤醒。
 * 首发前先睡 12 tick：确保消费者（先创建、先运行，但可能被抢占）在空缓冲上
 * recv 阻塞后再开始生产，演示交错确定化（否则生产者在消费者 recv 前先 send，
 * 消费者就不会 recv-block）。12 tick 足够轮转调度把每个就绪进程跑一遍。 */
__attribute__((section(".text.msg")))
void user_msg_p(void) {
    syscall3(SYS_MSG_CREATE, MSG_PIPE, 4, 0);
    sys_print("[MP] producer started, pid=");
    putdec(syscall3(SYS_GET_PID, 0, 0, 0));
    sys_print("\n");
    syscall3(SYS_SLEEP, 12, 0, 0);       /* 等消费者先 recv 阻塞 */
    for (int i = 0; i < MSG_N; i++) {
        syscall3(SYS_MSG_SEND, MSG_PIPE, (uint32_t)i, 0);
        sys_print("[MP] sent val=");
        putdec((uint32_t)i);
        sys_print("\n");
        syscall3(SYS_SLEEP, 1, 0, 0);    /* 快生产：1 tick */
    }
    sys_print("[MP] done\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}

/* ---- v0.8 文件系统演示 ---- */
#define FS_DEMO_SIZE 8000   /* 跨 2 个数据块，验证多块写入/读回 */

static void fill_pattern(uint32_t off, char *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (char)('a' + (off + i) % 26);
}

/* procFSA：创建文件 -> 分块写入 -> 关闭 -> 重开读回 -> 逐字节校验（闭环验证） */
__attribute__((section(".text.fs")))
void user_fs_w(void) {
    sys_print("[FA] fs write/read demo pid=");
    putdec(syscall3(SYS_GET_PID, 0, 0, 0));
    sys_print("\n");

    int ino = (int)syscall3(SYS_FS_CREATE, (uint32_t)"hello.txt", 0, 0);
    sys_print("[FA] create hello.txt inode=");
    putdec((uint32_t)ino);
    sys_print("\n");

    syscall3(SYS_FS_OPEN, 1, (uint32_t)"hello.txt", 1);   /* 写模式，槽 1 */
    uint32_t off = 0;
    while (off < FS_DEMO_SIZE) {
        char buf[64];
        uint32_t n = FS_DEMO_SIZE - off;
        if (n > 64) n = 64;
        fill_pattern(off, buf, n);
        if (syscall3(SYS_FS_WRITE, 1, (uint32_t)buf, n) != n) {
            sys_print("[FA] write fail\n");
            break;
        }
        off += n;
    }
    sys_print("[FA] wrote ");
    putdec(off);
    sys_print(" bytes\n");
    syscall3(SYS_FS_CLOSE, 1, 0, 0);

    syscall3(SYS_FS_OPEN, 2, (uint32_t)"hello.txt", 0);   /* 读模式，槽 2 */
    uint32_t rpos = 0, bad = 0;
    for (;;) {
        char buf[64], expect[64];
        uint32_t n = syscall3(SYS_FS_READ, 2, (uint32_t)buf, 64);
        if (n == 0) break;
        fill_pattern(rpos, expect, n);
        for (uint32_t i = 0; i < n; i++)
            if (buf[i] != expect[i]) bad++;
        rpos += n;
    }
    syscall3(SYS_FS_CLOSE, 2, 0, 0);

    sys_print("[FA] read back ");
    putdec(rpos);
    sys_print(" bytes, mismatches=");
    putdec(bad);
    sys_print("\n");
    if (rpos == FS_DEMO_SIZE && bad == 0)
        sys_print("[FA] verify OK\n");
    else
        sys_print("[FA] verify FAIL\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}

/* procFSB：建几个文件写一点内容，用 sys_fs_ls 列出根目录 */
__attribute__((section(".text.fs")))
void user_fs_l(void) {
    sys_print("[FL] ls demo pid=");
    putdec(syscall3(SYS_GET_PID, 0, 0, 0));
    sys_print("\n");

    syscall3(SYS_FS_CREATE, (uint32_t)"alpha.txt", 0, 0);
    syscall3(SYS_FS_CREATE, (uint32_t)"beta.txt", 0, 0);
    if (syscall3(SYS_FS_OPEN, 3, (uint32_t)"alpha.txt", 1) == 0)
        syscall3(SYS_FS_WRITE, 3, (uint32_t)"hello ls!", 9);
    syscall3(SYS_FS_CLOSE, 3, 0, 0);

    syscall3(SYS_FS_LS, 0, 0, 0);          /* 内核打印根目录 */
    sys_print("[FL] ls done\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
