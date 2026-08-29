/* mini-os/v2-c-kernel/sched.c
 * 进程调度器实现（v0.5）。
 *
 * 设计要点：
 *  1) PCB 0 是内核空闲进程(idle)，ring0 运行，只有"无任何就绪进程"时才被切入；
 *     用户进程从 PID 1 起，共享同一份 userprog.bin 代码页（映射在 0x80000000），
 *     各自拥有独立的内核栈与用户栈。
 *  2) 切换机制：中断进入 isr_common_stub 后在进程的内核栈上构造 registers_t 帧，
 *     调度器只需换 esp 并 ret 到 resume_point，即可恢复另一进程的现场。
 *  3) 就绪队列只放"可运行但未运行"的进程；退出/阻塞的进程由状态字段标记。
 */
#include "sched.h"
#include "sched_policy.h"
#include "usermode.h"
#include "timer.h"
#include "mem.h"
#include "serial.h"
#include "vga.h"
#include "kb.h"
#include "userprog_offsets.h"
#include <stdint.h>

#define USER_CODE_BASE   0x80000000u   /* 所有进程共享的代码页虚拟基址 */
#define KSTACK_SIZE      4096u

static pcb_t procs[MAX_PROCS];
static uint32_t current_pid = PID_KERNEL_IDLE;
static policy_readyq_t readyq;

/* v0.12: 定义在文件后部；sched_exec 需在定义前调用它 */
static void schedule(registers_t *r);

/* 分配一个空闲进程槽（pid）。v0.12: 进程退出（reap 置 FREE）后槽位可重用——
 * fork/exec 演示会产生更多并发进程，若按 next_pid 单调递增会在 MAX_PROCS 处耗尽。 */
static int alloc_pid(void) {
    for (uint32_t i = 1; i < MAX_PROCS; i++)
        if (procs[i].state == PROC_FREE) return (int)i;
    return -1;
}

extern char _binary_userprog_bin_start[];
extern char _binary_userprog_bin_end[];

/* v0.11: 共享代码页物理帧（每个进程在自己的页目录里映射到 0x80000000） */
static uint32_t user_code_phys = 0;

/* v0.13: 每个进程的用户栈区虚拟基址：代码页之后，按 pid 错开一个 8KB 槽。
 * 槽内 [基址, +4KB) 为守卫页（不映射，栈溢出陷阱），[+4KB, +8KB) 为栈页（映射）。 */
static uint32_t user_stack_vbase(uint32_t pid) {
    return USER_STACK_AREA_BASE + pid * USER_STACK_SLOT;
}

static void memcpy8(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}
static void memset8(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

/* v0.11: 拷贝进程名到内核内存。父进程传入的 name 可能位于其用户地址空间
 * （如 shell 的 "hello" 字符串常量），而子进程退出/回收时 CR3 是子进程
 * 自己的页目录，直接读父进程地址空间会缺页，故必须拷入内核自有缓冲区。 */
static void set_name(pcb_t *p, const char *name) {
    int i = 0;
    if (name)
        while (name[i] && i < (int)sizeof(p->name_buf) - 1) {
            p->name_buf[i] = name[i];
            i++;
        }
    p->name_buf[i] = 0;
    p->name = p->name_buf;
}

/* 在进程内核栈顶构造"初始中断现场"：sched_switch_esp 切到 gs 槽后
 * jmp resume_point，经 pop gs.. 与 iret 进入目标（ring0 或 ring3） */
static void frame_build(pcb_t *p, uint32_t cs, uint32_t ss, uint32_t eip,
                        uint32_t user_esp) {
    uint32_t faddr = p->kstack_top - sizeof(registers_t);  /* gs 槽 */
    registers_t *f = (registers_t *)faddr;
    f->gs = f->fs = f->es = f->ds = ss;
    f->edi = f->esi = f->ebp = f->esp = f->ebx = f->edx = f->ecx = f->eax = 0;
    f->int_no = 0;
    f->err_code = 0;
    f->eip = eip;
    f->cs  = cs;
    f->eflags = 0x200;        /* IF=1：iret 后开中断 */
    f->user_esp = user_esp;
    f->ss  = ss;
    p->kernel_esp = faddr;
}

/* 内核空闲进程：ring0，hlt 等待中断，负责状态刷新与键盘回显 */
static uint32_t idle_last_refresh = 0;
static void kernel_idle(void) {
    serial_puts("[sched] idle loop started\n");
    for (;;) {
        __asm__ volatile ("sti; hlt");   /* 中断唤醒，继续刷新状态 */
        if (ticks - idle_last_refresh >= 100) {
            idle_last_refresh = ticks;
            vga_row(2, 0x0A, "ticks:%u up:%u.%02us alive:%u",
                    (uint32_t)ticks, ticks / 100, ticks % 100, sched_alive_count());
            serial_printf("[tick] ticks=%u alive=%u free=%uKB\n",
                          (uint32_t)ticks, sched_alive_count(), free_memory_kb());
        }
        int c = kb_getchar();
        if (c >= 0) {
            vga_putc((char)c);
            serial_putc((char)c);
            if (c == '\n') serial_putc('\r');
        }
    }
}

void sched_init(void) {
    /* 共享代码页：所有进程复用同一份 userprog.bin */
    uint32_t size = (uint32_t)(_binary_userprog_bin_end - _binary_userprog_bin_start);
    user_code_phys = frame_alloc();
    if (!user_code_phys) {
        serial_puts("[sched] FATAL: no frame for code page\n");
        __asm__ volatile ("cli; hlt");
    }
    memcpy8((void *)user_code_phys, _binary_userprog_bin_start, size);
    map_page(USER_CODE_BASE, user_code_phys, 0x7);   /* P|RW|U，内核页目录也映射（兜底） */
    serial_printf("[sched] code %u bytes @%x (phys %x)\n", size,
                  USER_CODE_BASE, user_code_phys);

    /* 进程 0：内核 idle（ring0），使用内核页目录（page_dir=0） */
    pcb_t *idle = &procs[PID_KERNEL_IDLE];
    memset8((uint8_t *)idle, 0, sizeof(pcb_t));
    idle->pid = PID_KERNEL_IDLE;
    set_name(idle, "idle");
    idle->state = PROC_RUNNING;
    idle->kstack_frame = frame_alloc();
    idle->kstack_top = idle->kstack_frame + KSTACK_SIZE;
    idle->entry_off = 0;
    idle->user_esp_top = user_stack_vbase(0) + KSTACK_SIZE;
    idle->page_dir = 0;                              /* 内核页目录 */
    frame_build(idle, SEL_KCODE, SEL_KDATA, (uint32_t)&kernel_idle,
                idle->kstack_top);

    policy_readyq_init(&readyq);
}

int sched_spawn(uint32_t entry_off, const char *name) {
    int pid = alloc_pid();
    if (pid < 0) return -1;
    pcb_t *p = &procs[pid];
    memset8((uint8_t *)p, 0, sizeof(pcb_t));

    p->kstack_frame = frame_alloc();   /* 独立内核栈 */
    p->stack_frame  = frame_alloc();   /* 独立用户栈 */
    p->page_dir     = addr_space_create();   /* v0.11: 独立地址空间 */
    if (!p->kstack_frame || !p->stack_frame || !p->page_dir) {
        if (p->kstack_frame) frame_free(p->kstack_frame);
        if (p->stack_frame)  frame_free(p->stack_frame);
        if (p->page_dir)     addr_space_destroy(p->page_dir);
        serial_printf("[sched] spawn %s FAILED (OOM)\n", name);
        return -1;
    }

    p->kstack_top = p->kstack_frame + KSTACK_SIZE;
    uint32_t usv = user_stack_vbase(pid);         /* 栈区基址（守卫页） */
    uint32_t stk = usv + USER_STACK_GUARD;        /* 栈页（守卫页之后） */
    p->user_esp_top = usv + USER_STACK_SLOT;      /* 栈区顶 */
    /* v0.11: 共享代码页映射；v0.13: 只映射栈页，守卫页不映射（栈溢出陷阱） */
    map_page_in(p->page_dir, USER_CODE_BASE, user_code_phys, 0x7);
    map_page_in(p->page_dir, stk, p->stack_frame, 0x7);

    p->pid = pid;
    p->parent_pid = current_pid;   /* v0.14: 记录父进程（boot 演示为 idle=0） */
    set_name(p, name);
    p->entry_off = entry_off;
    p->state = PROC_READY;
    frame_build(p, SEL_UCODE_R3, SEL_UDATA_R3,
                USER_CODE_BASE + entry_off, p->user_esp_top);

    policy_readyq_push(&readyq, pid);

    serial_printf("[sched] spawn pid=%u name=%s entry=%x kstack=%x usp=%x pd=%x\n",
                  pid, name, USER_CODE_BASE + entry_off,
                  p->kstack_frame, p->user_esp_top, p->page_dir);
    return (int)pid;
}

int sched_spawn_at(uint32_t entry, const char *name, uint32_t pd,
                   const uint32_t *frames, uint32_t fcount, uint32_t vbase) {
    int pid = alloc_pid();
    if (pid < 0) return -1;
    pcb_t *p = &procs[pid];
    memset8((uint8_t *)p, 0, sizeof(pcb_t));

    p->kstack_frame = frame_alloc();   /* 独立内核栈 */
    p->stack_frame  = frame_alloc();   /* 独立用户栈 */
    p->page_dir     = pd;              /* v0.11: 由调用方建好的地址空间 */
    if (!p->kstack_frame || !p->stack_frame || !pd) {
        if (p->kstack_frame) frame_free(p->kstack_frame);
        if (p->stack_frame)  frame_free(p->stack_frame);
        serial_printf("[sched] spawn_at %s FAILED (OOM)\n", name);
        return -1;
    }

    p->kstack_top = p->kstack_frame + KSTACK_SIZE;
    uint32_t usv = user_stack_vbase(pid);         /* 栈区基址（守卫页） */
    uint32_t stk = usv + USER_STACK_GUARD;        /* 栈页 */
    p->user_esp_top = usv + USER_STACK_SLOT;      /* 栈区顶 */
    map_page_in(pd, stk, p->stack_frame, 0x7);    /* 独立用户栈页（守卫页不映射） */

    p->pid = pid;
    p->parent_pid = current_pid;   /* v0.14: 记录父进程 */
    set_name(p, name);
    p->entry_off = 0;
    p->state = PROC_READY;
    p->own_fcount = fcount;
    p->own_vbase = vbase;
    for (uint32_t i = 0; i < fcount && i < 8; i++) p->own_frames[i] = frames[i];

    frame_build(p, SEL_UCODE_R3, SEL_UDATA_R3, entry, p->user_esp_top);

    policy_readyq_push(&readyq, pid);

    serial_printf("[sched] spawn_at pid=%u name=%s entry=%x frames=%u pd=%x\n",
                  pid, p->name, entry, fcount, pd);
    return (int)pid;
}

/* ---- v0.12 fork / exec ----
 * fork：复制当前进程（用户地址空间深拷贝，共享内存保持共享），子进程从调用点继续。
 * exec：用新程序替换当前进程（配合 fork 实现经典 fork+exec+argv 模型）。 */

/* 释放进程"私有数据帧"（不含内核栈 kstack_frame）：
 * ELF 代码帧 + sys_map_page 私有页 + fork 深拷贝帧 + 用户栈帧。
 * terminate/reap/exec 复用；调用方负责 addr_space_destroy 与 kstack_frame。 */
static void release_priv_frames(pcb_t *p) {
    for (uint32_t i = 0; i < p->own_fcount && i < 8; i++) frame_free(p->own_frames[i]);
    p->own_fcount = 0;
    for (uint32_t i = 0; i < p->map_fcount && i < 8; i++) frame_free(p->map_frames[i]);
    p->map_fcount = 0;
    for (uint32_t i = 0; i < p->fork_fcount && i < 24; i++) frame_free(p->fork_frames[i]);
    p->fork_fcount = 0;
    if (p->stack_frame) { frame_free(p->stack_frame); p->stack_frame = 0; }
}

int sched_fork(registers_t *r) {
    int pid = alloc_pid();
    if (pid < 0) return -1;
    pcb_t *c = &procs[pid];
    memset8((uint8_t *)c, 0, sizeof(pcb_t));
    pcb_t *p = &procs[current_pid];

    c->kstack_frame = frame_alloc();
    if (!c->kstack_frame) return -1;
    c->page_dir = addr_space_create();
    if (!c->page_dir) { frame_free(c->kstack_frame); return -1; }

    /* 深拷贝父进程用户半区（PDE 512..1023）除共享内存区外的所有映射页。
     * 页表帧/页目录帧都落在低 16MB 恒等映射区，任何地址空间都能直接读写。 */
    uint32_t *dir = (uint32_t *)mem_current_pd();   /* 父进程页目录 = 当前 CR3 */
    for (uint32_t i = 512; i < 1024; i++) {
        if (!(dir[i] & 1)) continue;
        uint32_t *pt = (uint32_t *)(dir[i] & ~0xFFFu);
        for (uint32_t j = 0; j < 1024; j++) {
            if (!(pt[j] & 1)) continue;
            uint32_t virt = (i << 22) | (j << 12);
            uint32_t phys = pt[j] & ~0xFFFu;
            uint32_t flags = pt[j] & 0xFFFu;
            if (virt >= SHMEM_VBASE && virt < SHMEM_VBASE + SHMEM_SLOTS * 0x1000u) {
                /* 共享内存页：保持同一物理帧（fork 后父子共享该页，不深拷贝） */
                map_page_in(c->page_dir, virt, phys, flags);
                continue;
            }
            if (c->fork_fcount >= 24) goto fork_oom;
            uint32_t nf = frame_alloc();
            if (!nf) goto fork_oom;
            memcpy8((void *)nf, (const void *)phys, 4096);   /* 内容拷贝 */
            map_page_in(c->page_dir, virt, nf, flags);
            c->fork_frames[c->fork_fcount++] = nf;
        }
    }

    /* 子进程现场 = 父进程当前中断帧副本，eax 置 0（子进程 fork 返回 0）。
     * 用户 esp 指向父进程用户栈——子进程已深拷贝该栈，同一虚拟地址内容一致，
     * 子进程从 fork 调用点继续执行（局部变量/调用链与原样）。 */
    c->kstack_top = c->kstack_frame + KSTACK_SIZE;
    uint32_t faddr = c->kstack_top - sizeof(registers_t);
    memcpy8((void *)faddr, (const void *)r, sizeof(registers_t));
    ((registers_t *)faddr)->eax = 0;

    c->pid = pid;
    c->parent_pid = p->pid;   /* v0.14: fork 子进程的父进程 = 当前（父）进程 */
    c->state = PROC_READY;
    set_name(c, p->name);
    c->entry_off = 0;
    c->kernel_esp = faddr;
    c->user_esp_top = p->user_esp_top;
    c->own_vbase = p->own_vbase;
    c->block_reason = BLOCK_NONE;
    /* own_frames/map_frames 已清零：深拷贝出的帧统一记在 fork_frames，退出时回收 */

    policy_readyq_push(&readyq, pid);
    serial_printf("[fork] pid=%u -> child=%u pd=%x frames=%u\n",
                  p->pid, pid, c->page_dir, c->fork_fcount);
    return (int)pid;

fork_oom:
    for (uint32_t i = 0; i < c->fork_fcount && i < 24; i++) frame_free(c->fork_frames[i]);
    addr_space_destroy(c->page_dir);
    frame_free(c->kstack_frame);
    serial_printf("[fork] pid=%u fork FAILED (OOM)\n", p->pid);
    return -1;
}

/* 在新用户栈顶（虚拟 [usv, usv+4K)，物理帧 sframe）按 cdecl 布置 argv 块：
 * 从高地址到低地址：字符串区 -> argv 指针数组(n+1) -> argc 槽 -> argv 指针槽 -> fake_ret(esp)。
 * 进入 app_main 时 [esp]=返回地址, [esp+4]=argc, [esp+8]=argv（指针数组地址）。
 * 返回 "esp" 虚拟地址。 */
static uint32_t argv_layout(uint32_t sframe, uint32_t usv,
                            const char (*argv)[64], uint32_t argc) {
    uint32_t n = argc > 8 ? 8 : argc;
    uint32_t cur = usv + 4096;               /* 从栈顶向下布置 */
    uint32_t off[8];
    for (int i = (int)n - 1; i >= 0; i--) {  /* 字符串：倒序放，低地址在前 */
        uint32_t len = 0;
        while (argv[i][len] && len < 63) len++;
        cur -= len + 1;
        char *dst = (char *)(sframe + (cur - usv));
        for (uint32_t k = 0; k <= len; k++) dst[k] = argv[i][k];
        off[i] = cur - usv;
    }
    cur -= (n + 1) * 4;                      /* argv 指针数组 */
    uint32_t argv_v = cur;
    uint32_t *ptrs = (uint32_t *)(sframe + (cur - usv));
    for (uint32_t i = 0; i < n; i++) ptrs[i] = usv + off[i];
    ptrs[n] = 0;
    cur -= 4;                                /* argv 指针槽（esp+8，更高地址） */
    *(uint32_t *)(sframe + (cur - usv)) = argv_v;
    cur -= 4;                                /* argc 槽（esp+4） */
    *(uint32_t *)(sframe + (cur - usv)) = n;
    cur -= 4;                                /* fake_ret（esp 指向） */
    *(uint32_t *)(sframe + (cur - usv)) = 0;
    return cur;                              /* esp */
}

int sched_exec(registers_t *r, const char *name, uint32_t pd,
               uint32_t entry, const uint32_t *frames, uint32_t fcount, uint32_t vbase,
               const char (*argv)[64], uint32_t argc) {
    pcb_t *p = &procs[current_pid];
    uint32_t usv = user_stack_vbase(p->pid);      /* 栈区基址（守卫页） */
    uint32_t stk = usv + USER_STACK_GUARD;        /* 栈页 */

    /* name 可能指向当前进程用户内存（如 shell 栈上的参数字符串），
     * 而下面会释放旧地址空间，故先拷贝进 PCB 的 name_buf。 */
    set_name(p, name);

    uint32_t sframe = frame_alloc();
    if (!sframe) return -1;
    memset8((void *)sframe, 0, 4096);
    map_page_in(pd, stk, sframe, 0x7);            /* 新用户栈页（守卫页不映射） */
    uint32_t esp = argv_layout(sframe, stk, argv, argc);

    /* 释放旧用户资源（ELF 代码/私有页/fork 帧/旧栈/旧页目录）；内核栈保留复用 */
    release_priv_frames(p);
    addr_space_destroy(p->page_dir);
    p->page_dir = pd;
    switch_page_dir(pd);                     /* 立即切到新地址空间（旧 pd 已释放） */

    /* PCB 换成新程序（pid 与内核栈 kstack_frame 不变） */
    p->stack_frame = sframe;
    p->own_fcount = fcount;
    for (uint32_t i = 0; i < fcount && i < 8; i++) p->own_frames[i] = frames[i];
    p->own_vbase = vbase;
    p->entry_off = 0;
    p->block_reason = BLOCK_NONE;

    /* 就地改写当前中断帧为新程序入口现场：
     * schedule() 保存的 kernel_esp 即 r，下次切回时 iret 从新入口执行。
     * user_esp 指向 argv 块（cdecl: [esp]=ret,[esp+4]=argc,[esp+8]=argv）。
     * 调用方须保持关中断（exec 关键段），schedule 切走，由 iret 开中断。 */
    r->edi = r->esi = r->ebp = r->esp = r->ebx = r->edx = r->ecx = r->eax = 0;
    r->eip = entry;
    r->cs  = SEL_UCODE_R3;
    r->eflags = 0x200;                        /* IF=1 */
    r->user_esp = esp;
    r->ss  = SEL_UDATA_R3;
    r->int_no = 0; r->err_code = 0;
    serial_printf("[exec] pid=%u -> '%s' entry=%x argv_esp=%x\n",
                  p->pid, p->name, entry, esp);
    schedule(r);                              /* 切走；下次切回执行新程序 */
    __asm__ volatile ("cli; hlt");            /* 不可达 */
    return 0;
}

/* ---- 核心：挑选下一进程并切换（调度点都不返回） ---- */
static void schedule(registers_t *r) {
    pcb_t *cur = &procs[current_pid];
    cur->kernel_esp = (uint32_t)r;   /* 保存当前进程现场指针 */

    /* 运行中的进程放回就绪队尾（idle 特殊：只在无就绪进程时兜底） */
    if (cur->state == PROC_RUNNING && cur->pid != PID_KERNEL_IDLE) {
        cur->state = PROC_READY;
        policy_readyq_push(&readyq, cur->pid);
    }

    if (policy_readyq_empty(&readyq)) {
        if (cur->pid == PID_KERNEL_IDLE) return;   /* 本就空闲，无需切换 */
        pcb_t *idle = &procs[PID_KERNEL_IDLE];
        idle->state = PROC_RUNNING;
        current_pid = PID_KERNEL_IDLE;
        usermode_set_esp0(idle->kstack_top);
        switch_page_dir(idle->page_dir);   /* 0 -> 内核页目录 */
        sched_switch_esp(idle->kernel_esp);
        __asm__ volatile ("cli; hlt");   /* 不可达 */
    }

    uint32_t next = policy_readyq_pop(&readyq);
    pcb_t *n = &procs[next];
    n->state = PROC_RUNNING;
    current_pid = next;
    usermode_set_esp0(n->kstack_top);
    switch_page_dir(n->page_dir);   /* v0.11: 切到目标进程的地址空间（CR3） */
    sched_switch_esp(n->kernel_esp);
    __asm__ volatile ("cli; hlt");       /* 不可达 */
}

void sched_start(void) {
    if (policy_readyq_empty(&readyq)) {
        pcb_t *idle = &procs[PID_KERNEL_IDLE];
        idle->state = PROC_RUNNING;
        current_pid = PID_KERNEL_IDLE;
        usermode_set_esp0(idle->kstack_top);
        switch_page_dir(idle->page_dir);
        serial_puts("[sched] no process, going idle\n");
        sched_switch_esp(idle->kernel_esp);
    }
    uint32_t next = policy_readyq_pop(&readyq);
    pcb_t *n = &procs[next];
    n->state = PROC_RUNNING;
    current_pid = next;
    usermode_set_esp0(n->kstack_top);
    switch_page_dir(n->page_dir);   /* v0.11: 切入第一个进程的地址空间 */
    serial_printf("[sched] start -> pid=%u name=%s\n", n->pid, n->name);
    sched_switch_esp(n->kernel_esp);
    __asm__ volatile ("cli; hlt");       /* 不可达 */
}

/* ---- 僵尸回收 ----
 * 回收僵尸进程的内核栈/用户栈/页目录并置 FREE。父进程 sys_wait 拿到退出码后
 * 调 sched_reap；无父进程的僵尸由 sched_tick 心跳回收。 */
static void reap_process(uint32_t i) {
    pcb_t *p = &procs[i];
    frame_free(p->kstack_frame);
    frame_free(p->stack_frame);
    addr_space_destroy(p->page_dir);   /* v0.11: 释放进程独占页表 + 页目录 */
    p->page_dir = 0;
    serial_printf("[sched] reap pid=%u name=%s code=%u\n",
                  p->pid, p->name ? p->name : "?", p->exit_code);
    p->state = PROC_FREE;
}

void sched_reap(uint32_t pid) {
    if (pid >= MAX_PROCS || procs[pid].state != PROC_ZOMBIE) return;
    reap_process(pid);
}

/* 定时器心跳：唤醒到期阻塞进程 -> 回收僵尸 -> 抢占切换 */
void sched_tick(registers_t *r) {
    for (uint32_t i = 1; i < MAX_PROCS; i++) {
        pcb_t *p = &procs[i];
        /* 只唤醒"定时 sleep"阻塞的进程；sem 等待由 sem_signal 显式唤醒，
         * 防止把信号量等待者按 wakeup_tick 误唤 */
        if (p->state == PROC_BLOCKED && p->block_reason == BLOCK_SLEEP &&
            (int32_t)(ticks - p->wakeup_tick) >= 0) {
            p->state = PROC_READY;
            p->block_reason = BLOCK_NONE;
            policy_readyq_push(&readyq, p->pid);
            serial_printf("[sched] wake pid=%u at tick=%u\n", p->pid, (uint32_t)ticks);
        }
    }
    /* 回收僵尸进程资源（不会回收当前运行进程）。
     * v0.14 延迟回收：只有"没有父进程会 wait"的僵尸才由心跳回收——
     *   父进程为 0（boot 演示/孤儿），或父进程已 FREE。
     * 否则保留僵尸，等父进程 sys_wait 时回收并取得退出码，
     * 修复"spawn 后、父进程 wait 前被抢先回收"导致 wait 返回 -1 的竞态。 */
    for (uint32_t i = 1; i < MAX_PROCS; i++) {
        pcb_t *p = &procs[i];
        if (p->state != PROC_ZOMBIE) continue;
        if (p->parent_pid != 0) {
            pcb_t *par = &procs[p->parent_pid];
            if (par->pid == p->parent_pid && par->state != PROC_FREE) continue;
        }
        reap_process(i);
    }
    /* schedule() 仅在"当前为 idle 且就绪队列为空"时返回；
     * 此时必须正常返回（不可 cli;hlt），让 iret 回到 idle 循环
     * 继续刷新状态并再次 hlt 等待下一个心跳。 */
    schedule(r);
}

void sched_yield(registers_t *r) {
    schedule(r);
    /* 同上：idle 空队列让出时正常返回，勿停机 */
}

void sched_sleep(registers_t *r, uint32_t n) {
    pcb_t *p = &procs[current_pid];
    p->state = PROC_BLOCKED;
    p->block_reason = BLOCK_SLEEP;
    p->wakeup_tick = ticks + n;
    serial_printf("[sched] sleep pid=%u %u ticks (wake@%u)\n",
                  p->pid, n, p->wakeup_tick);
    schedule(r);
    __asm__ volatile ("cli; hlt");       /* 不可达 */
}

/* 阻塞当前进程等待事件（信号量/消息队列等）。reason 记录阻塞原因，arg 记录阻塞参数；
 * 现场保存在中断帧上，将来被 sched_wake/sched_wake_with 唤醒后由 iret 恢复用户态 */
void sched_block(registers_t *r, uint32_t reason, uint32_t arg) {
    pcb_t *p = &procs[current_pid];
    p->state = PROC_BLOCKED;
    p->block_reason = reason;
    p->block_arg = arg;
    serial_printf("[sched] block pid=%u reason=%u arg=%u\n", p->pid, reason, arg);
    schedule(r);
    __asm__ volatile ("cli; hlt");       /* 不可达 */
}

/* 唤醒阻塞进程：置就绪并入队；同时把保存帧的 eax 置为指定值，
 * 使被唤醒进程的阻塞系统调用按语义返回（sleep/sem 返回 0；msg_recv 返回消息值） */
void sched_wake_with(uint32_t pid, uint32_t eax_val) {
    if (pid >= MAX_PROCS) return;
    pcb_t *p = &procs[pid];
    if (p->state != PROC_BLOCKED) {
        serial_printf("[sched] wake pid=%u skipped (state=%u)\n",
                      pid, p->state);
        return;
    }
    registers_t *f = (registers_t *)p->kernel_esp;
    if (f) f->eax = eax_val;
    p->state = PROC_READY;
    p->block_reason = BLOCK_NONE;
    policy_readyq_push(&readyq, pid);
    serial_printf("[sched] wake pid=%u\n", pid);
}

void sched_wake(uint32_t pid) { sched_wake_with(pid, 0); }

/* v0.9: 键盘行完成时由 kb 行回调调用，唤醒等待 sys_readline 的进程（取第一个）。
 * 直接把行拷入等待者的用户缓冲区，并把 syscall 返回值置为行长度。
 * v0.11: 该缓冲区属于"等待者"的地址空间，而当前地址空间可能是 idle 或别的进程，
 * 故临时把 CR3 切到等待者的页目录完成拷贝，再切回（行缓冲在低内存恒等映射区，
 * 任何地址空间都可读）。 */
void sched_wake_keyboard(void) {
    for (uint32_t i = 1; i < MAX_PROCS; i++) {
        pcb_t *p = &procs[i];
        if (p->state == PROC_BLOCKED && p->block_reason == BLOCK_KEYBOARD) {
            char *out = (char *)p->block_arg;
            uint32_t max = p->block_arg2 ? p->block_arg2 : KB_LINE_MAX + 1;
            uint32_t saved_pd = mem_current_pd();
            if (p->page_dir && p->page_dir != saved_pd)
                switch_page_dir(p->page_dir);
            int n = kb_line_take(out, max);
            switch_page_dir(saved_pd);
            sched_wake_with(p->pid, (uint32_t)(n < 0 ? 0 : n));
            serial_printf("[sched] wake keyboard waiter pid=%u (%d bytes)\n",
                          p->pid, n);
            return;
        }
    }
}

static void terminate_current(registers_t *r, uint32_t code, const char *why) {
    pcb_t *p = &procs[current_pid];
    p->exit_code = code;
    p->state = PROC_ZOMBIE;
    /* v0.9/v0.11/v0.12: 回收本进程私有数据帧（ELF 代码/私有页/fork 深拷贝帧/用户栈） */
    release_priv_frames(p);
    /* v0.15: 父进程退出 -> 子进程孤儿化（parent_pid=0，交心跳回收）。
     * 否则父的 pid 槽被复用后，孤儿永远等不到"父进程 FREE"而被回收。 */
    for (uint32_t i = 1; i < MAX_PROCS; i++)
        if (i != current_pid && procs[i].parent_pid == current_pid)
            procs[i].parent_pid = 0;
    /* v0.15: 唤醒等待本进程退出的父进程（sys_wait 阻塞者）：
     *  - 等待具体 pid（waitpid(pid)）或等待任意子进程（wait()，block_arg=-1）
     *  - 返回子进程 pid；退出码写入等待者的 *status 出参（切到其地址空间写入） */
    for (uint32_t i = 1; i < MAX_PROCS; i++) {
        pcb_t *q = &procs[i];
        if (q->state != PROC_BLOCKED || q->block_reason != BLOCK_WAIT) continue;
        if (q->block_arg != p->pid && q->block_arg != (uint32_t)-1) continue;
        uint32_t *status = (uint32_t *)q->block_arg2;
        if (status) {
            uint32_t saved_pd = mem_current_pd();
            if (q->page_dir && q->page_dir != saved_pd)
                switch_page_dir(q->page_dir);
            *status = p->exit_code;
            switch_page_dir(saved_pd);
        }
        sched_wake_with(q->pid, p->pid);   /* sys_wait 返回子进程 pid */
        serial_printf("[sched] wake waiter pid=%u (child %u exit code=%u)\n",
                      q->pid, p->pid, p->exit_code);
        p->parent_pid = 0;   /* v0.14: 退出码已交付给父进程，僵尸交心跳回收 */
        break;
    }
    serial_printf("[sched] %s pid=%u name=%s code=%u\n", why,
                  p->pid, p->name ? p->name : "?", code);
    vga_printf("[sched] %s pid=%u (%s) code=%u\n", why, p->pid,
               p->name ? p->name : "?", code);
    schedule(r);
    __asm__ volatile ("cli; hlt");       /* 不可达 */
}

void sched_exit(registers_t *r, uint32_t code) {
    terminate_current(r, code, "exit");
}

void sched_kill(registers_t *r, uint32_t code) {
    terminate_current(r, code, "kill");
}

uint32_t sched_current_pid(void) { return current_pid; }
pcb_t   *sched_get(uint32_t pid)  { return pid < MAX_PROCS ? &procs[pid] : 0; }

uint32_t sched_alive_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != PROC_FREE) n++;
    return n;
}
