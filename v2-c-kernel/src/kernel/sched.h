/* mini-os/v2-c-kernel/sched.h
 * 进程调度器（v0.5）：
 *  - PCB 表 + 就绪队列（轮转策略见 sched_policy.c）
 *  - 定时器抢占、主动让出(yield)、sleep 阻塞、退出/故障终止、僵尸回收
 *  - 每个进程独立内核栈；切换复用中断现场（registers_t 帧），
 *    通过 isr.s 中的 resume_point 恢复，无需保存额外寄存器。
 */
#ifndef _SCHED_H
#define _SCHED_H
#include <stdint.h>
#include "idt.h"
#include "mem.h"   /* v0.26: USER_STACK_PAGES（PCB.stack_frames 长度） */

#define MAX_PROCS       16
#define PID_KERNEL_IDLE 0     /* 进程 0 固定为内核空闲进程 */

typedef enum {
    PROC_FREE,      /* 槽位空闲 */
    PROC_READY,     /* 就绪（在就绪队列中） */
    PROC_RUNNING,   /* 运行中（不在就绪队列） */
    PROC_BLOCKED,   /* 阻塞（不在就绪队列） */
    PROC_ZOMBIE     /* 已退出，待回收（不在就绪队列） */
} pstate_t;

/* 阻塞原因（PCB 字段，区分定时唤醒与事件唤醒，防止误唤） */
typedef enum {
    BLOCK_NONE = 0,
    BLOCK_SLEEP,    /* 定时 sleep：由 sched_tick 按 wakeup_tick 唤醒 */
    BLOCK_SEM,      /* 信号量等待：由 sem_signal 显式唤醒 */
    BLOCK_MSG,      /* 消息队列等待：由 msg_send/recv 显式唤醒 */
    BLOCK_KEYBOARD, /* v0.9: 等待键盘输入一行（由 kb 行完成回调唤醒） */
    BLOCK_WAIT      /* v0.9: 等待指定子进程退出（sys_wait） */
} block_reason_t;

typedef struct {
    uint32_t pid;
    uint32_t parent_pid;     /* v0.14: 父进程 pid（0=无父进程/boot 演示，僵尸由心跳回收；
                                否则父进程存活时保留僵尸，等父进程 sys_wait 回收并取退出码） */
    pstate_t state;
    const char *name;
    char name_buf[16];     /* v0.11: 进程名拷贝到内核内存（父进程字符串在其地址空间，
                              子进程退出/回收时 CR3 是它自己的页目录，不能直接读） */
    uint32_t kernel_esp;     /* 保存的中断现场指针（指向 gs 槽） */
    uint32_t kstack_top;     /* 本进程内核栈顶（写 tss.esp0 用） */
    uint32_t kstack_frame;   /* 内核栈占用的物理帧（回收用） */
    /* v0.26 用户栈按需生长：槽 32KB = 槽底守卫页（永不映射）+ 可生长栈区。
     * stack_frames[] 记已映射栈页的物理帧（回收用，不含守卫页）；
     * stack_bottom 为最低已映射栈页的虚拟地址（守卫页在其下 4K，随生长下移）。 */
    uint32_t stack_frames[USER_STACK_PAGES];
    uint32_t stack_fcount;   /* 上表有效项数 */
    uint32_t stack_bottom;   /* 最低已映射栈页的虚拟地址（栈向下生长） */
    /* v0.26#2 用户堆（brk）：堆区 [USER_HEAP_BASE, heap_brk) 已按需映射（页对齐）。
     * heap_frames[] 记已映射堆页的物理帧（回收用）；收缩只更新 heap_brk 保留映射。 */
    uint32_t heap_frames[USER_HEAP_PAGES];
    uint32_t heap_fcount;    /* 上表有效项数 */
    uint32_t heap_brk;       /* 当前 program break（堆顶，虚拟地址） */
    uint32_t heap_base;      /* 堆起点（= USER_HEAP_BASE，创建进程时置位） */
    uint32_t entry_off;      /* 用户入口在 userprog.bin 中的偏移 */
    uint32_t user_esp_top;   /* 本进程用户栈顶（iret 帧的 esp） */
    uint32_t exit_code;
    uint32_t wakeup_tick;    /* BLOCK_SLEEP: 苏醒时刻 */
    uint32_t block_reason;   /* 阻塞原因（BLOCK_*） */
    uint32_t block_arg;      /* 阻塞参数（如消息队列 id / 等待的子进程 pid / readline 缓冲区） */
    uint32_t block_arg2;     /* v0.9: 阻塞辅助参数（如 readline 缓冲区上限） */
    uint32_t *own_frames;   /* v0.9+0.26#3: 从文件加载的用户代码物理帧列表（kmalloc 动态数组，
                               退出时回收；不再受 8 帧/32KB 上限约束，支持 MB 级 ELF） */
    uint32_t own_fcount;     /* 上表有效项数 */
    uint32_t own_vbase;      /* 该代码区虚拟基址（判定 app 槽占用用） */
    uint32_t map_frames[8];  /* v0.11: sys_map_page 用户申请的物理页（退出时回收） */
    uint32_t map_fcount;     /* 上表有效项数 */
    uint32_t page_dir;       /* v0.11: 本进程页目录物理地址；idle 为 0（内核页目录） */
    uint32_t fork_frames[24];/* v0.12: sys_fork 深拷贝出的用户页物理帧（退出时回收）。
                                USER_CODE/用户栈/ELF 代码/私有页都深拷贝；共享内存区保持共享不在此列 */
    uint32_t fork_fcount;    /* 上表有效项数 */
} pcb_t;

/* 由 isr.s 提供：无条件切到目标 esp 并 ret（不返回） */
extern void sched_switch_esp(uint32_t esp);
/* 由 isr.s 提供：中断现场恢复点（pop gs.. -> iret） */
extern void resume_point(void);

void sched_init(void);
int  sched_spawn(uint32_t entry_off, const char *name); /* 返回 pid 或 -1 */
/* v0.9: 以绝对入口地址创建进程，并可携带"从文件加载的用户代码帧"（退出时回收）。
 * v0.11: pd 为本进程页目录（调用方已建好并映射好共享代码/ELF 页；0 表示暂用内核页目录） */
int  sched_spawn_at(uint32_t entry, const char *name, uint32_t pd,
                    const uint32_t *frames, uint32_t fcount, uint32_t vbase);
void sched_start(void);      /* 切入第一个就绪进程（不返回） */
/* v0.12: fork 当前进程（用户地址空间深拷贝，共享内存保持共享）。
 * 父进程返回子进程 pid；子进程从调用点继续（eax=0）。 */
int  sched_fork(registers_t *r);
/* v0.12: 用新程序替换当前进程（exec）。argv 为内核缓冲中的参数字符串数组
 * （argv[0..argc-1] 各以 \0 结尾，最多 8 条）。成功不返回；失败返回 -1（调用方回滚）。 */
int  sched_exec(registers_t *r, const char *name, uint32_t pd,
                uint32_t entry, const uint32_t *frames, uint32_t fcount, uint32_t vbase,
                const char (*argv)[64], uint32_t argc);
void sched_tick(registers_t *r);  /* 定时器心跳：唤醒阻塞 + 抢占 + 回收 */
void sched_yield(registers_t *r); /* 主动让出（不返回） */
void sched_sleep(registers_t *r, uint32_t ticks);  /* 阻塞若干心跳（不返回） */
void sched_block(registers_t *r, uint32_t reason, uint32_t arg); /* 阻塞当前进程等待事件（不返回） */
void sched_wake(uint32_t pid);                    /* 唤醒阻塞进程入就绪队列（eax=0） */
void sched_wake_with(uint32_t pid, uint32_t eax); /* 唤醒并指定其系统调用返回值 */
void sched_wake_keyboard(void);                   /* v0.9: 键盘行完成时唤醒 readline 等待者 */
void sched_exit(registers_t *r, uint32_t code);    /* 正常退出当前进程（不返回） */
void sched_kill(registers_t *r, uint32_t code);    /* 故障终止当前进程（不返回） */
/* v0.14: 立即回收指定僵尸进程的资源并置 FREE（父进程 sys_wait 时调用）。
 * 普通退出仍由 sched_tick 心跳回收；本函数供 wait 路径"拿到退出码后及时释放"。 */
void sched_reap(uint32_t pid);

uint32_t sched_current_pid(void);
pcb_t   *sched_get(uint32_t pid);
uint32_t sched_alive_count(void);

/* v0.21 内核自审计：PCB 状态机合法性检查，返回失败项数（0=通过） */
uint32_t sched_audit(void);

#endif
