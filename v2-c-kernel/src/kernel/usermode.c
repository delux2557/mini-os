/* mini-os/v2-c-kernel/usermode.c
 * 用户态支持：
 *  1) 重建 GDT：kernel cs/ds(0x08/0x10) + user cs/ds(0x18/0x20) + TSS(0x28)
 *  2) 初始化 TSS：ss0/esp0 指向内核栈，供中断时从 ring3 切回 ring0；
 *     esp0 由调度器在每次进程切换时更新
 *  3) 系统调用分发：int 0x80 门，eax=号, ebx/ecx/edx=参数；
 *     涉及调度（退出/睡眠/让出）的调用不返回
 */
#include "usermode.h"
#include "vga.h"
#include "serial.h"
#include "idt.h"
#include "timer.h"
#include "sched.h"
#include "sem.h"
#include "msg.h"
#include "blockdev.h"
#include "fs.h"
#include "mem.h"
#include "heap.h"
#include "brk.h"
#include "elf.h"
#include "kb.h"
#include "storage.h"
#include "userptr.h"
#include "netsock.h"
#include "netio.h"
#include <stddef.h>
#include <stdint.h>

/* ---- v0.6 IPC/同步：内核信号量表（用户通过固定 id 引用，id 0 保留） ---- */
#define SEM_MAX_OBJ 16
typedef struct {
    int  used;
    sem_t sem;
} sem_obj_t;
static sem_obj_t sem_objects[SEM_MAX_OBJ];

/* ---- v0.7 IPC：内核有界消息队列表（用户通过固定 id 引用，id 0 保留） ---- */
#define MSG_MAX_OBJ 8
typedef struct {
    int   used;
    msgq_t q;
} msg_obj_t;
static msg_obj_t msg_objects[MSG_MAX_OBJ];

/* 共享内存页：所有进程映射到同一固定虚拟地址（同一物理帧）即可互通。
 * slot 0..3 落在 0x80020000 之后（避开用户栈区 0x8001xxxx）。
 * 基址/槽数在 mem.h 定义（v0.12：fork 需据此识别共享页以跳过深拷贝）。 */
static uint32_t shmem_phys[SHMEM_SLOTS];

/* ---- v0.31（per-process fd）：每进程打开文件表入 PCB（fs_file_t 定义移至 sched.h）。
 * v0.8-v0.30 为全局 fs_files[8] 表——跨进程槽号互污染、异常退出泄漏（BUG-031）。
 * 改造后 fd 号是"本进程内约定号"：打开/读写/关闭都在当前进程自己的 fd 表上做，
 * 并发进程互不影响，退出/exec 只清自己的表。 */
static fs_file_t *cur_fdt(void) {
    return sched_get(sched_current_pid())->fd_table;
}

/* ---- v0.9 从文件系统加载 ELF 应用 ----
 * 应用链接到 APP_LINK（0x800A0000，v0.26#3 app 区扩到 1MB），加载时用 mapfn
 * 逐页分配物理帧并映射；退出时由调度器回收（pcb.own_frames，v0.26#3 动态数组）。
 * shell 常驻 SHELL_LINK（0x80090000），帧不随退出回收。
 * v0.26#3: load_frames 由固定 8 项静态数组改为按需 kmalloc 的动态列表，
 * 不再受 32KB/8 帧上限约束（ELF 加载去上限）。 */
#define APP_ELF_MAXSIZE 0x100000u   /* 单个 ELF 文件上限：1MB（app 区同量级） */

static uint32_t load_vbase;        /* mapfn 边界检查：ELF 实际最低链接地址（页对齐） */
static uint32_t load_region;       /* 覆盖范围字节数 = load_maxframes 页 */
static uint32_t *load_frames;      /* v0.26#3: 各页物理帧（kmalloc 动态数组，load_maxframes 项） */
static uint32_t load_fcount;
static uint32_t load_maxframes;
static uint32_t load_pd;           /* v0.11: 本次加载的目标进程页目录 */
static int      load_failed;

/* 释放本次加载的帧记账数组（帧本身已移交 PCB 或另行回收） */
static void load_frames_free(void) {
    if (load_frames) { kfree(load_frames); load_frames = NULL; }
}

/* elf_load 的映射钩子：为目标虚拟区分配物理帧并映射进"目标进程页目录"。
 * v0.11: 加载期间 CR3 已被切到目标页目录，故只映射进 load_pd 即可，
 * 段数据由 elf_load 直接写入目标地址空间，不再污染当前（父进程）页目录。
 * 若父进程与子进程链接到同一虚拟地址，旧方案（临时映射进当前页目录）会
 * 覆盖父进程自身映射导致其恢复运行时缺页。 */
static int app_mapfn(uint32_t vaddr, uint32_t len) {
    if (load_failed) return -1;
    uint32_t start = vaddr & 0xFFFFF000u;
    uint32_t end = (vaddr + len + 0xFFFu) & 0xFFFFF000u;
    if (start < load_vbase || end > load_vbase + load_region) { load_failed = 1; return -1; }
    for (uint32_t pg = start; pg < end; pg += 0x1000) {
        if (load_fcount >= load_maxframes) { load_failed = 1; return -1; }
        uint32_t phys = frame_alloc();
        if (!phys) { load_failed = 1; return -1; }
        /* OBS-009：页表帧 OOM（map_page_in 返 -1）→ 释放刚分配的数据帧并中止加载，
         * 否则该页静默未映射、运行到才缺页崩（与 BUG-033 栈生长处理一致）。不把 phys
         * 记入 load_frames，避免 load_elf_file 失败清理时 double free。 */
        if (map_page_in(load_pd, pg, phys, 0x7) != 0) {
            frame_free(phys);
            load_failed = 1;
            return -1;
        }
        load_frames[load_fcount] = phys;
        load_fcount++;
    }
    return 0;
}

/* 从文件系统读取 ELF 并加载到指定区域，成功返回 0 并把入口写入 *entry */
static int load_elf_file(const char *name, uint32_t vbase, uint32_t *entry) {
    blockdev_t *bd = fs_device();
    int ino = fs_lookup(bd, name);
    if (ino < 0) { serial_printf("[elf] '%s' not found\n", name); return -1; }
    uint32_t sz = fs_size(bd, (uint32_t)ino);
    if (sz == 0 || sz > APP_ELF_MAXSIZE) { serial_printf("[elf] '%s' bad size %u\n", name, sz); return -1; }

    uint8_t *buf = (uint8_t *)kmalloc(sz);
    if (!buf) return -1;
    if (fs_read(bd, (uint32_t)ino, buf, 0, sz) != (int)sz) { kfree(buf); return -1; }

    /* 映射区间 = ELF 自带 PT_LOAD 范围（页对齐），vbase 仅用于日志/登记。
     * 注意 ELF 头页在 -Ttext 地址的前一页，region 必须含它，否则 mapfn 拒绝、拷贝缺页。 */
    uint32_t lbase, lend;
    if (elf_load_range(buf, sz, &lbase, &lend) != 0) { kfree(buf); return -1; }
    /* BUG-056/审计：钳制加载区间。p_memsz 由文件头声称、与文件大小脱钩，畸形值会让
     * mapfn 单次申请巨大映射（拖死/耗尽帧）。强制：区间≤1MB 且整体落在用户半区。 */
    if (lbase < USER_SPACE_BASE || lend > USER_SPACE_END ||
        lend - lbase > APP_ELF_MAXSIZE) { kfree(buf); return -1; }
    load_vbase = lbase;
    load_region = lend - lbase;
    load_maxframes = load_region / 0x1000u;
    load_frames = (uint32_t *)kmalloc(load_maxframes * sizeof(uint32_t));
    if (!load_frames) { kfree(buf); return -1; }
    load_fcount = 0;
    load_failed = 0;
    (void)vbase;
    int rc = elf_load(buf, sz, 0, app_mapfn, entry);   /* 0=按链接地址原样放置 */
    kfree(buf);
    if (rc != 0 || load_failed) {
        for (uint32_t i = 0; i < load_fcount; i++) frame_free(load_frames[i]);
        load_frames_free();
        load_fcount = 0;
        serial_printf("[elf] load '%s' failed rc=%d\n", name, rc);
        return -1;
    }
    serial_printf("[elf] '%s' loaded %u bytes @%x entry=%x (%u frames)\n",
                  name, sz, lbase, *entry, load_fcount);
    return 0;
}

int usermode_spawn_elf(const char *name, uint32_t vbase, int resident) {
    /* v0.11: 父进程传入的 name 可能位于其用户地址空间，而加载期间 CR3 会切到
     * 新进程页目录（克隆内核半区，不含父进程用户映射），故先拷到内核栈缓冲，
     * 之后所有日志/拷贝都用这份内核内存中的名字。
     * v0.35（红队 F1 修复）：按名/按路径加载缓冲统一 64B（path 约定），覆盖
     * "最深层级路径"且与其它 fs 接口（case 13/14/18 的 char path[64]）一致；
     * FS_MAX_NAME=24（单分量，含 NUL）。加载接口接受可含 '/' 的路径，故缓冲须 ≥ 路径
     * 上限而不只是单分量上限。**不允许再出现小于该上限的按名缓冲**（曾 16B 致
     * 16~23 字符合法程序名被静默截断撞名前缀、加载错误程序）。 */
    char namebuf[64];
    int ni = 0;
    if (name)
        while (name[ni] && ni < (int)sizeof(namebuf) - 1) { namebuf[ni] = name[ni]; ni++; }
    namebuf[ni] = 0;

    /* v0.11: 先为该进程建独立地址空间（克隆内核共享 PDE + 清空用户半区） */
    uint32_t pd = addr_space_create();
    if (!pd) {
        serial_printf("[elf] spawn '%s' FAILED (no page directory)\n", namebuf);
        return -1;
    }
    load_pd = pd;
    uint32_t entry;
    /* v0.11: 加载期间把 CR3 切到目标进程页目录，段数据由 elf_load 直接写入
     * 目标地址空间；关中断防止定时器抢占后以父进程 CR3 恢复执行导致缺页
     * （内核代码/栈/堆/ramdisk 均在低 16MB 恒等映射区，任何地址空间都可访问）。 */
    uint32_t saved_pd = mem_current_pd();
    __asm__ volatile ("cli");
    switch_page_dir(pd);
    int rc = load_elf_file(namebuf, vbase, &entry);
    switch_page_dir(saved_pd);
    /* v0.11 注意：这里不恢复 sti。引导期若过早开中断，定时器会抢占内核，
     * 在 shell 尚未注册、sched_start() 未执行时就让用户进程跑起来（引导乱序）。
     * 保持关中断：引导路径由 sched_start() 的 iret 开中断；syscall 路径由
     * iret 恢复用户 eflags（IF=1）。sched_spawn_at 不阻塞，安全。 */
    if (rc != 0) {
        addr_space_destroy(pd);
        return -1;
    }
    /* resident=1（shell）：帧常驻，不记入 pcb（退出时不回收） */
    int pid = sched_spawn_at(entry, namebuf, pd,
                             resident ? 0 : load_frames,
                             resident ? 0 : load_fcount, vbase);
    if (pid < 0) {
        for (uint32_t i = 0; i < load_fcount; i++) frame_free(load_frames[i]);
        load_frames_free();
        load_fcount = 0;
        addr_space_destroy(pd);
        serial_printf("[elf] spawn '%s' failed (no pid)\n", namebuf);
        return -1;
    }
    /* v0.11: 帧仅映射在目标进程页目录（进程运行时使用），当前地址空间无临时映射，
     * 无需清理；帧已移交 pcb（成功时）/已回收（失败时） */
    load_fcount = 0;
    load_frames_free();   /* v0.26#3: 记账数组用完即还（帧已移交 PCB） */
    return pid;
}

/* ---- GDT ---- */
struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* x86 32 位 TSS 结构（当 ring3 发生中断时，CPU 用 ss0/esp0 切到内核栈） */
struct tss_entry {
    uint32_t prev;
    uint32_t esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr gdtp;
static struct tss_entry tss;

static void memset8(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

/* ---- GDT 描述符设置 ---- */
static void gdt_set(int i, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t flags) {
    gdt[i].limit_lo     = (uint16_t)(limit & 0xFFFF);
    gdt[i].base_lo      = (uint16_t)(base & 0xFFFF);
    gdt[i].base_mid     = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].access       = access;
    gdt[i].granularity  = (uint8_t)((flags & 0xF0) | ((limit >> 16) & 0x0F));
    gdt[i].base_hi      = (uint8_t)((base >> 24) & 0xFF);
}

static void tss_set(int i) {
    uint32_t base = (uint32_t)&tss;
    uint32_t limit = (uint32_t)sizeof(struct tss_entry) - 1;
    gdt[i].limit_lo    = (uint16_t)(limit & 0xFFFF);
    gdt[i].base_lo     = (uint16_t)(base & 0xFFFF);
    gdt[i].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].access      = 0x89;   /* present, 32bit available TSS */
    gdt[i].granularity = 0;
    gdt[i].base_hi     = (uint8_t)((base >> 24) & 0xFF);
}

void usermode_init(void) {
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);   /* kcode 0x08, DPL0 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);   /* kdata 0x10, DPL0 */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);   /* ucode 0x18, DPL3 */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);   /* udata 0x20, DPL3 */
    tss_set(5);                            /* tss   0x28 */

    gdtp.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtp.base  = (uint32_t)gdt;
    __asm__ volatile ("lgdt %0" : : "m"(gdtp));

    /* 远跳转刷新 CS，随后重载数据段 */
    __asm__ volatile ("ljmp $0x08, $1f\n1:" ::: "memory");
    __asm__ volatile ("mov $0x10, %%ax; mov %%ax, %%ds; mov %%ax, %%es; "
                      "mov %%ax, %%fs; mov %%ax, %%gs; mov %%ax, %%ss"
                      ::: "eax");

    /* TSS：esp0 由调度器按进程更新；先置为内核栈顶兜底 */
    memset8(&tss, 0, sizeof(tss));
    tss.ss0  = SEL_KDATA;
    tss.esp0 = 0;
    __asm__ volatile ("mov $0x28, %%ax; ltr %%ax" ::: "eax");
}

void usermode_set_esp0(uint32_t esp0) { tss.esp0 = esp0; }

/* ---- v0.21 内核自审计 ----
 * 由 selftest 一键触发：物理帧配平（mem_audit）+ 堆完整性（heap_audit）
 * + 信号量不变量（sem_invariant_ok）+ PCB 状态机（sched_audit）。
 * 各子系统打印一行 [audit] 日志（serial+vga），
 * 本函数汇总返回失败检查项总数（0=全部通过）。 */
static uint32_t kern_audit(void) {
    uint32_t bad = 0;
    bad += mem_audit();
    bad += heap_audit();
    bad += sched_audit();
    bad += netsock_audit();          /* v0.31 socket 表不变量（含 DHCP 保留槽恒计数） */
    /* RD5（BUG-071）：块归属冲突/违约累计次数——仅观测、不入 bad（防守成功的"被阻断"
     * 不是健康失效，正常引导恒 0）。非 0 即镜像存在"范围内重复块"（含 RO 块被别名）。 */
    serial_printf("[audit] fs_owner: %u violations\n", fs_owner_violations_get());
    uint32_t objs = 0;
    for (uint32_t i = 1; i < SEM_MAX_OBJ; i++) {
        if (!sem_objects[i].used) continue;
        objs++;
        if (!sem_invariant_ok(&sem_objects[i].sem)) {
            serial_printf("[audit] sem FAIL: id=%u count=%d waiters=%u\n",
                          i, sem_objects[i].sem.count, sem_objects[i].sem.wait_count);
            vga_printf("[audit] sem FAIL: id=%u count=%d waiters=%u\n",
                       i, sem_objects[i].sem.count, sem_objects[i].sem.wait_count);
            bad++;
        }
    }
    if (objs == 0)
        serial_printf("[audit] sem ok: no semaphores\n");
    else if (bad == 0)
        serial_printf("[audit] sem ok: %u objects\n", objs);
    return bad;
}

/* ---- 系统调用分发（int 0x80 门进入） ---- */
/* ---- L1（栈预算总账）：把大缓冲 case 下沉为独立函数 ----
 * syscall_dispatch 是 int 0x80 唯一入口，任何 syscall 都会在其栈帧上运行。若把
 * exec 的 names[8][64](512B) / sendto 的 pbuf[1400] 直接写进 dispatch 的 switch，
 * 编译器按最大 case 预留栈帧（实测 2224B），叠加下层 recvfrom/netsock_send 深链后
 * 逼近甚至越过 4KB 内核栈预算（SEC-07 教训：DHCP 链 5528B 静默写穿）。
 * 改为独立函数：单个 case 的大缓冲只在自身栈帧出现，dispatch 单帧回落到 <300B，
 * 各深链（exec/sendto/recvfrom…）互不叠加，都在 check_stack_budget.sh 预算内。 */

/* case 25 sys_exec(name, argc, argv)：argv 快照 names[8][64] 独立成帧。
 * noinline：若被 -O2 内联回 dispatch，大缓冲又会抬高分发器单帧，拆分失去意义。 */
static __attribute__((noinline)) void sys_exec_case(registers_t *r, uint32_t a, uint32_t b, uint32_t c) {
    if (b > 8) { r->eax = (uint32_t)-1; return; }
    uint32_t argc = b;
    char *const *argv = (char *const *)c;
    /* v0.17：name / argv 数组 / 每个 argv[i] 字符串都先校验并拷入内核缓冲。
     * name 与 argv 内容都位于当前（旧）地址空间，而加载/替换期间会切 CR3，
     * 故须先在当前地址空间把它们全部拷入内核缓冲（与 spawn_elf 的 namebuf 同因）。
     * v0.35（红队 F1 修复）：name 缓冲 64B（path 约定，见 spawn_elf 注释），
     * 用 copyin_str_full 在超长时显式失败而非静默截断（防撞名前缀误加载）。 */
    char namebuf[64];
    if (copyin_str_full((const char *)a, namebuf, sizeof(namebuf)) < 0) { r->eax = (uint32_t)-1; return; }
    if (argc && !user_ptr_valid((const void *)argv, argc * sizeof(char *))) {
        r->eax = (uint32_t)-1;
        return;
    }
    char names[8][64];
    for (uint32_t i = 0; i < argc; i++) {
        if (!argv[i]) { r->eax = (uint32_t)-1; return; }
        if (copyin_str(argv[i], names[i], sizeof(names[i])) < 0) { r->eax = (uint32_t)-1; return; }
    }
    /* 加载 ELF 到新地址空间（与 spawn_elf 同法：建 pd、切 CR3 加载、切回）。
     * 全程关中断：sched_exec 释放旧地址空间期间不允许抢占。 */
    uint32_t pd = addr_space_create();
    if (!pd) { r->eax = (uint32_t)-1; return; }
    load_pd = pd;
    uint32_t entry;
    uint32_t saved_pd = mem_current_pd();
    __asm__ volatile ("cli");
    switch_page_dir(pd);
    int rc = load_elf_file(namebuf, APP_LINK, &entry);
    switch_page_dir(saved_pd);
    if (rc != 0) {
        addr_space_destroy(pd);
        __asm__ volatile ("sti");
        serial_printf("[user] exec '%s' load failed\n", namebuf);
        r->eax = (uint32_t)-1;
        return;
    }
    rc = sched_exec(r, namebuf, pd, entry, load_frames, load_fcount, load_vbase,
                    names, argc);       /* 成功不返回 */
    if (rc != 0) {
        for (uint32_t i = 0; i < load_fcount; i++) frame_free(load_frames[i]);
        load_frames_free();        /* v0.28 审查修复：归还 load_frames 数组（P0-2） */
        addr_space_destroy(pd);
        load_fcount = 0;
        __asm__ volatile ("sti");
        serial_printf("[user] exec '%s' failed (no stack)\n", namebuf);
        r->eax = (uint32_t)-1;
        return;
    }
    /* 成功路径（sched_exec 不返回）里 load_frames 数组已由 sched_exec 复制后释放 */
    load_fcount = 0;
    __asm__ volatile ("cli; hlt");   /* 不可达 */
}

/* case 31 sys_net_sendto(sock, *iov)：发送缓冲 pbuf[1400] 独立成帧（同 sys_exec_case 的 noinline 理由） */
static __attribute__((noinline)) void sys_sendto_case(registers_t *r, uint32_t a, uint32_t b) {
    struct net_send_iov iov;
    if (copyin((const void *)b, &iov, sizeof(iov)) < 0) { r->eax = (uint32_t)-1; return; }
    if (iov.len > 1400) { r->eax = (uint32_t)-1; return; }
    uint8_t pbuf[1400];
    if (iov.len && copyin(iov.buf, pbuf, iov.len) < 0) { r->eax = (uint32_t)-1; return; }
    int n = netsock_send((int)a, iov.dst_ip, iov.dst_port, pbuf, iov.len);
    serial_printf("[net] sendto sock=%d %uB -> %x:%u rc=%d\n",
                  (int)a, iov.len, iov.dst_ip, iov.dst_port, n);
    r->eax = (uint32_t)n;
}

/* case 18 sys_fs_ls(path)：目录列表缓冲 ents[64×32=2048B] 是 dispatch 内现存最大
 * 单帧来源，独立成帧（同 sys_exec_case 的 noinline 理由/栈预算总账）。 */
static __attribute__((noinline)) void sys_fs_ls_case(registers_t *r, uint32_t a) {
    char path[64];
    const char *pp;
    if (a == 0) { pp = ""; }
    else if (copyin_str((const char *)a, path, sizeof(path)) < 0) { r->eax = (uint32_t)-1; return; }
    else { pp = path; }
    fs_dir_entry_t ents[FS_MAX_INODES];
    int n = fs_list(fs_device(), pp, ents, FS_MAX_INODES);
    if (n < 0) { r->eax = (uint32_t)-1; return; }
    vga_printf("[ls] %s:\n", pp[0] ? pp : "/");
    serial_printf("[ls] %s:\n", pp[0] ? pp : "/");
    for (int i = 0; i < n; i++) {
        uint32_t sz = fs_size(fs_device(), ents[i].inode);
        const char *mark = ents[i].type == FS_TYPE_DIR ? "/" : "";
        vga_printf("  %s%s (inode=%u size=%u)\n", ents[i].name, mark,
                   ents[i].inode, sz);
        serial_printf("[ls]   %s%s inode=%u size=%u\n", ents[i].name, mark,
                      ents[i].inode, sz);
    }
    vga_printf("[ls] %d entries\n", n);
    serial_printf("[ls] %d entries\n", n);
    r->eax = (uint32_t)n;
}

void syscall_dispatch(registers_t *r) {
    /* 只允许来自用户态（ring3）的触发 */
    if ((r->cs & 3) != 3) {
        serial_printf("[syscall] rejected from ring %u\n", r->cs & 3);
        r->eax = (uint32_t)-1;
        return;
    }
    uint32_t num = r->eax;
    uint32_t a = r->ebx, b = r->ecx, c = r->edx;

    /* ---- A-2 ② 参数语义表（BUG-067 延续，逐类处理原则）----
     * syscall 入参全是 uint32（ebx/ecx/edx）。分五类，原则如下：
     * ① id/slot/fd 类：一律先 `a==0 || a>=N` 下界(0 保留为守护/未用)+上界检查，越界 -1
     *   （如 sem/msg=1..SEM_MAX_OBJ-1、fs fd=1..FS_FDS_PER_PROC-1、net id<NET_SOCK_MAX）。
     *   负 fd 经符号回绕成巨大 uint32，被同一条 `a>=N` 上界检查挡住，无需单列。
     * ② 指针/缓冲类：user_ptr_valid 验**整区**、copyin/copyin_str 拷入内核缓冲后才用；
     *   输出参数 copyout 回写前同样先验目标区（v0.17 起全链路）。越界/非法一律 -1。
     * ③ 长度/容量类：统一按无符号 uint32 接收，**使用前必须钳位**到真实内核缓冲/表容量
     *   （如 recvfrom max→1400、IP 载荷 plen→NET_RXMAX、readline max→KB_LINE_MAX）——
     *   防"负长回绕成巨大 uint32"打穿 memcpy/循环；负长即被钳位吞掉，不会越界。
     * ④ 有符号语义类（负值无合法含义者）：显式按 int32 解释并拒绝，如 sem_create init<0
     *   → -1（SEM-1 审计误报根因，本轮修复）；不静默截断/clamp，fail-closed。
     * ⑤ 地址/偏移类：brk 由 brk_in_range 钳上界(USER_HEAP_MAX)与下界(heap_base)；
     *   fs_seek 按无符号原生存 pos，越界由后续 read/write 的越界哨兵 fail-closed（见 case 26）。
     * 维护规则：**新增 syscall 时按①②③④⑤对照自查**——无符号化收参给 ③，若要无符号化而
     * 语义需拒绝负值走 ④；凡"长度/容量"入参必须 clamp，凡指针必须整区校验。 */

    /* BUG-058 per-process syscall 掩码（最小权限）：num>=64 直接拒（防 1ull<<num 移位 UB）；
     * 被禁用则 -1。置于任何 copyin/参数解析之前，尽早拦截（被掩码的 syscall 不解析参数）。
     * 注意：只判断"是否被本进程禁用"，不拦截 SYS_LIMIT 本身（除非该进程连 36 都禁了）。 */
    {
        pcb_t *curp = sched_get(sched_current_pid());
        if (curp && curp->sc_mask && num < 64 && (curp->sc_mask & (1ull << num))) {
            serial_printf("[syscall] pid=%u masked syscall %u\n", curp->pid, num);
            r->eax = (uint32_t)-1;
            return;
        }
    }

    switch (num) {
    case 0:   /* sys_exit(code) */
        serial_printf("[user] sys_exit(%u) pid=%u\n", a, sched_current_pid());
        sched_exit(r, a);
        __asm__ volatile ("cli; hlt");   /* 不可达 */
        return;
    case 1:   /* sys_print(string)：先拷贝进内核缓冲（v0.17 校验用户指针） */
        {
            char sbuf[256];
            if (copyin_str((const char *)a, sbuf, sizeof(sbuf)) < 0) {
                r->eax = (uint32_t)-1;
                return;
            }
            vga_puts(sbuf);
            serial_puts(sbuf);
        }
        r->eax = 0;
        return;
    case 2:   /* sys_get_ticks() */
        r->eax = ticks;
        return;
    case 3:   /* sys_sleep(ticks) */
        sched_sleep(r, a);
        __asm__ volatile ("cli; hlt");   /* 不可达 */
        return;
    case 4:   /* sys_yield() */
        sched_yield(r);
        __asm__ volatile ("cli; hlt");   /* 不可达 */
        return;
    case 5:   /* sys_get_pid() */
        r->eax = sched_current_pid();
        return;
    case 6:   /* sys_sem_create(id, init)：在固定 id 槽创建/获取信号量 */
        if (a == 0 || a >= SEM_MAX_OBJ) { r->eax = (uint32_t)-1; return; }
        /* A-2 ② SEM-1：init 按 int32 解释，负数（含"误传 -1"这类哨兵）会令 sem 计数为负，
         * 触发 sem_invariant_ok 审计误报且无合法语义——显式拒绝（fail-closed，打日志）。 */
        if ((int32_t)b < 0) {
            serial_printf("[sem] create id=%u DENIED negative init=%d\n", a, (int32_t)b);
            r->eax = (uint32_t)-1;
            return;
        }
        if (!sem_objects[a].used) {
            sem_objects[a].used = 1;
            sem_init(&sem_objects[a].sem, (int32_t)b);
            serial_printf("[sem] create id=%u init=%d\n", a, (int32_t)b);
        }
        r->eax = a;
        return;
    case 7: { /* sys_sem_wait(id)：占用资源，必要时阻塞当前进程（不返回） */
        if (a == 0 || a >= SEM_MAX_OBJ || !sem_objects[a].used) {
            r->eax = (uint32_t)-1; return;
        }
        uint32_t pid = sched_current_pid();
        int rc = sem_wait_try(&sem_objects[a].sem, pid);
        if (rc == 0) {
            serial_printf("[sem] wait pid=%u id=%u ok\n", pid, a);
            r->eax = 0;
            return;
        }
        if (rc == 1) {   /* 资源不足，已入队，阻塞本进程 */
            r->eax = 0;  /* 预先写好返回值，唤醒后由 iret 恢复 */
            serial_printf("[sem] wait pid=%u id=%u -> block\n", pid, a);
            sched_block(r, BLOCK_SEM, a);
            __asm__ volatile ("cli; hlt");   /* 不可达 */
        }
        r->eax = (uint32_t)-1;   /* 等待队列满 */
        return;
    }
    case 8: { /* sys_sem_signal(id)：释放资源，唤醒队首等待者 */
        if (a == 0 || a >= SEM_MAX_OBJ || !sem_objects[a].used) {
            r->eax = (uint32_t)-1; return;
        }
        uint32_t wpid = sem_signal_wake(&sem_objects[a].sem);
        if (wpid != SEM_NO_PID) {
            serial_printf("[sem] signal id=%u -> wake pid=%u\n", a, wpid);
            sched_wake(wpid);
        } else {
            serial_printf("[sem] signal id=%u ok (count++)\n", a);
        }
        r->eax = 0;
        return;
    }
    case 9: { /* sys_shmem(slot)：确保共享页已映射，返回其虚拟地址。
                * v0.11: 每个进程独立地址空间，物理帧只在首次分配，
                * 但必须"每次都"把共享页映射进当前进程的页目录，
                * 否则第二个进程再调用时会因缺页崩溃。
                * 首次分配时清零页面（符合匿名共享内存语义）。 */
        if (a >= SHMEM_SLOTS) { r->eax = 0; return; }
        if (!shmem_phys[a]) {
            shmem_phys[a] = frame_alloc();
            if (!shmem_phys[a]) { r->eax = 0; return; }
            uint32_t *p = (uint32_t *)shmem_phys[a];
            for (int i = 0; i < 1024; i++) p[i] = 0;   /* 清零 */
            serial_printf("[sem] shmem slot=%u alloc phys=%x (zeroed)\n",
                          a, shmem_phys[a]);
        }
        map_page(SHMEM_VBASE + a * 0x1000, shmem_phys[a], 0x7);  /* 当前进程页目录 */
        r->eax = SHMEM_VBASE + a * 0x1000;
        return;
    }
    case 10:  /* sys_msg_create(id, capacity)：在固定 id 槽创建有界消息队列 */
        if (a == 0 || a >= MSG_MAX_OBJ) { r->eax = (uint32_t)-1; return; }
        if (!msg_objects[a].used) {
            msg_objects[a].used = 1;
            msg_init(&msg_objects[a].q, (uint32_t)b);
            serial_printf("[msg] create id=%u capacity=%u\n", a, (uint32_t)b);
        }
        r->eax = a;
        return;
    case 11: { /* sys_msg_send(id, value)：发消息；缓冲满则阻塞本生产者（不返回） */
        if (a == 0 || a >= MSG_MAX_OBJ || !msg_objects[a].used) {
            r->eax = (uint32_t)-1; return;
        }
        uint32_t pid = sched_current_pid();
        int rc = msg_send_try(&msg_objects[a].q, b, pid);
        if (rc == 0) {   /* 入队成功：若有等待消费者，直接把消息交给它 */
            uint32_t outv;
            uint32_t wpid = msg_send_wake(&msg_objects[a].q, &outv);
            if (wpid != MSG_NO_PID) {
                serial_printf("[msg] send id=%u -> handoff consumer pid=%u val=%u\n",
                              a, wpid, outv);
                sched_wake_with(wpid, outv);   /* 消费者 recv 直接返回该消息 */
            }
            serial_printf("[msg] send pid=%u id=%u -> ok\n", pid, a);
            r->eax = 0;
            return;
        }
        if (rc == 1) {   /* 缓冲满：消息已暂存，阻塞本生产者（唤醒后 send 视为成功） */
            serial_printf("[msg] send pid=%u id=%u -> block (full)\n", pid, a);
            sched_block(r, BLOCK_MSG, a);
            __asm__ volatile ("cli; hlt");   /* 不可达 */
        }
        r->eax = (uint32_t)-1;   /* 生产者等待队列满 */
        return;
    }
    case 12: { /* sys_msg_recv(id)：收消息；缓冲空则阻塞本消费者（不返回） */
        if (a == 0 || a >= MSG_MAX_OBJ || !msg_objects[a].used) {
            r->eax = (uint32_t)-1; return;
        }
        uint32_t pid = sched_current_pid();
        uint32_t val;
        int rc = msg_recv_try(&msg_objects[a].q, &val, pid);
        if (rc == 0) {   /* 取出成功：若有暂存生产者且有空位，搬入并唤醒它 */
            uint32_t wpid = msg_recv_wake(&msg_objects[a].q);
            if (wpid != MSG_NO_PID) {
                serial_printf("[msg] recv id=%u -> wake producer pid=%u\n", a, wpid);
                sched_wake(wpid);   /* 生产者 send 返回 0=成功 */
            }
            serial_printf("[msg] recv pid=%u id=%u -> val=%u\n", pid, a, val);
            r->eax = val;
            return;
        }
        if (rc == 1) {   /* 缓冲空：阻塞本消费者（唤醒后 recv 返回交棒消息） */
            serial_printf("[msg] recv pid=%u id=%u -> block (empty)\n", pid, a);
            sched_block(r, BLOCK_MSG, a);
            __asm__ volatile ("cli; hlt");   /* 不可达 */
        }
        r->eax = (uint32_t)-1;   /* 消费者等待队列满 */
        return;
    }
    case 13: { /* sys_fs_create(name)：根目录建文件，返回 inode 或 -1 */
        char path[64];
        if (copyin_str((const char *)a, path, sizeof(path)) < 0) { r->eax = (uint32_t)-1; return; }
        int ino = fs_create(fs_device(), path);
        serial_printf("[fs] create '%s' inode=%d\n", path, ino);
        r->eax = (uint32_t)ino;
        return;
    }
    case 14: { /* sys_fs_open(fd, name, mode)：fd 为本进程约定号；mode 0=只读 1=只写 2=追加(v0.14) */
        fs_file_t *fdt = cur_fdt();
        if (a == 0 || a >= FS_FDS_PER_PROC || fdt[a].used) { r->eax = (uint32_t)-1; return; }
        char path[64];
        if (copyin_str((const char *)b, path, sizeof(path)) < 0) { r->eax = (uint32_t)-1; return; }
        int ino = fs_lookup(fs_device(), path);
        if (ino < 0) { r->eax = (uint32_t)-1; return; }
        /* BUG-057：请求写/追加（c!=0）但目标为只读系统文件 -> 直接 -1（纵深；
         * fs_write 层仍有权威拦截兜底，避免"打开只读却到写时才被打回"）。 */
        if (c != 0 && fs_is_ro(fs_device(), (uint32_t)ino) == 1) { r->eax = (uint32_t)-1; return; }
        fdt[a].used  = 1;
        fdt[a].inode = (uint32_t)ino;
        fdt[a].pos   = (c == 2) ? fs_size(fs_device(), (uint32_t)ino) : 0;
        fdt[a].mode  = c;
        serial_printf("[fs] open fd=%u '%s' inode=%u mode=%u pos=%u (pid=%u)\n",
                      a, path, ino, c, fdt[a].pos, sched_current_pid());
        r->eax = 0;
        return;
    }
    case 15: { /* sys_fs_write(fd, buf, len)：从当前位置写（buf 须为合法用户指针） */
        fs_file_t *fdt = cur_fdt();
        if (a == 0 || a >= FS_FDS_PER_PROC || !fdt[a].used) { r->eax = (uint32_t)-1; return; }
        fs_file_t *f = &fdt[a];
        if (f->mode == 0) { r->eax = (uint32_t)-1; return; }   /* 只读槽不可写 */
        if (!user_ptr_valid((const void *)b, c)) { r->eax = (uint32_t)-1; return; }
        int n = fs_write(fs_device(), f->inode, (const void *)b, f->pos, c);
        if (n > 0) f->pos += (uint32_t)n;
        serial_printf("[fs] write fd=%u inode=%u pos=%u +%d\n", a, f->inode, f->pos, n);
        r->eax = (uint32_t)n;
        return;
    }
    case 16: { /* sys_fs_read(fd, buf, len)：从当前位置读（buf 须为合法用户指针） */
        fs_file_t *fdt = cur_fdt();
        if (a == 0 || a >= FS_FDS_PER_PROC || !fdt[a].used) { r->eax = (uint32_t)-1; return; }
        fs_file_t *f = &fdt[a];
        if (f->mode != 0) { r->eax = (uint32_t)-1; return; }
        if (!user_ptr_valid((const void *)b, c)) { r->eax = (uint32_t)-1; return; }
        int n = fs_read(fs_device(), f->inode, (void *)b, f->pos, c);
        if (n > 0) f->pos += (uint32_t)n;
        serial_printf("[fs] read fd=%u inode=%u pos=%u +%d\n", a, f->inode, f->pos, n);
        r->eax = (uint32_t)n;
        return;
    }
    case 17: { /* sys_fs_close(fd) */
        fs_file_t *fdt = cur_fdt();
        if (a == 0 || a >= FS_FDS_PER_PROC || !fdt[a].used) { r->eax = (uint32_t)-1; return; }
        fdt[a].used = 0;
        serial_printf("[fs] close fd=%u\n", a);
        r->eax = 0;
        return;
    }
    case 18:  /* sys_fs_ls(path)：列出目录内容（v0.14 路径化；path 空/0=根目录）。
                 L1：大缓冲 ents[2048B] 下沉 sys_fs_ls_case（栈预算总账） */
        sys_fs_ls_case(r, a);
        return;
    case 19: { /* sys_fs_delete(name)：删除文件（v0.14 支持路径；目录用 sys_fs_rmdir） */
        char path[64];
        if (copyin_str((const char *)a, path, sizeof(path)) < 0) { r->eax = (uint32_t)-1; return; }
        blockdev_t *bd = fs_device();
        int ino = fs_lookup(bd, path);        /* 解析实际对象（成功删除后供 revoke 悬垂 fd） */
        int rc = fs_delete(bd, path);
        serial_printf("[fs] delete '%s' rc=%d\n", path, rc);
        /* D4（红队 RBT-2026-014，BUG-068）：删除成功即回收指向该 inode 的悬垂 fd，
         * 防 inode 最低位复用后被旧 fd 写落到新文件（跨文件写、无告警）。 */
        if (rc == 0 && ino >= 0) sched_fd_revoke((uint32_t)ino);
        r->eax = (uint32_t)rc;
        return;
    }
    case 20: { /* sys_readline(buf, max)：阻塞式读一行；行已就绪则直接返回长度 */
        char *out = (char *)a;
        /* v0.36（红队 RBT-2026-013，BUG-067）：max=0 不是"未指定"哨兵。旧写 0 被吞成
         * KB_LINE_MAX+1(129)，会向"零容量"调用方缓冲整行写入（契约违反）。调用方必须给
         * 真实缓冲容量；0 直接显式失败，便于尽早暴露非法调用方，而不是静默越界写。 */
        if (b == 0) { r->eax = (uint32_t)-1; return; }
        uint32_t max = b;
        if (!user_ptr_valid(out, max)) { r->eax = (uint32_t)-1; return; }   /* v0.17 */
        if (!kb_line_ready()) {
            /* 无行：记录缓冲区（唤醒时由 sched_wake_keyboard 拷入），阻塞等待 */
            pcb_t *p = sched_get(sched_current_pid());
            if (p) p->block_arg2 = max;
            serial_printf("[kb] readline pid=%u -> block\n", sched_current_pid());
            sched_block(r, BLOCK_KEYBOARD, a);   /* block_arg = 用户缓冲区 */
            __asm__ volatile ("cli; hlt");       /* 不可达 */
        }
        int n = kb_line_take(out, max);
        serial_printf("[kb] readline pid=%u -> %d bytes\n", sched_current_pid(), n);
        r->eax = (uint32_t)n;
        return;
    }
    case 21: { /* sys_spawn_file(name)：从文件系统加载 ELF 应用到 app 槽，返回 pid */
        /* v0.35（红队 F1 修复）：name 缓冲 64B（path 约定）；copyin_str_full 使
         * 超长名字显式失败（返回 -2 → 本次 -1），不静默截断撞前名前缀误加载。 */
        char namebuf[64];
        if (copyin_str_full((const char *)a, namebuf, sizeof(namebuf)) < 0) {
            serial_printf("[user] spawn_file name too long/invalid\n");
            r->eax = (uint32_t)-1;
            return;
        }
        int pid = usermode_spawn_elf(namebuf, APP_LINK, 0);
        serial_printf("[user] spawn_file '%s' -> pid=%d\n", namebuf, pid);
        r->eax = (uint32_t)pid;
        return;
    }
    case 22: { /* sys_wait(pid, *status)：等待子进程退出（经典 wait/waitpid，v0.15）。
                  - pid = -1：等待"任意"子进程（wait()）
                  - pid 具体：等待该子进程（waitpid(pid)）
                  返回：被回收的子进程 pid；无子进程/非法参数返回 -1；
                  退出码写入 *status 出参（NULL 则丢弃）。
                  只回收"自己的"子进程（ch->parent_pid == 当前 pid）。 */
        uint32_t *status = (uint32_t *)b;
        uint32_t cur = sched_current_pid();
        if (status && !user_ptr_valid(status, sizeof(uint32_t))) {   /* v0.17 */
            r->eax = (uint32_t)-1;
            return;
        }

        /* 快速路径：已有已退出的子进程（ZOMBIE），立即回收 */
        if (a == (uint32_t)-1) {
            for (uint32_t i = 1; i < MAX_PROCS; i++) {
                pcb_t *c = sched_get(i);
                if (!c || c->state != PROC_ZOMBIE || c->parent_pid != cur) continue;
                uint32_t code = c->exit_code;
                serial_printf("[dbg] fastpath pid=%u st=%u ppid=%u exit=%u\n",
                              i, c->state, c->parent_pid, c->exit_code);
                if (status) *status = code;      /* 当前地址空间即父进程，直接写 */
                sched_reap(i);
                serial_printf("[user] wait any -> pid=%u code=%u (reaped)\n", i, code);
                r->eax = i;
                return;
            }
        } else {
            if (a >= MAX_PROCS) { r->eax = (uint32_t)-1; return; }
            pcb_t *ch = sched_get(a);
            if (!ch || ch->state == PROC_FREE || ch->parent_pid != cur) {
                r->eax = (uint32_t)-1;   /* 不存在 / 已回收 / 不是我的子进程 */
                return;
            }
            if (ch->state == PROC_ZOMBIE) {
                uint32_t code = ch->exit_code;
                if (status) *status = code;
                sched_reap(a);
                serial_printf("[user] wait pid=%u -> reaped code=%u\n", a, code);
                r->eax = a;
                return;
            }
        }
        /* 阻塞路径：目标子进程还活着 -> 阻塞等待其退出；无子进程可等 -> -1 */
        {
            int have_child = 0;
            for (uint32_t i = 1; i < MAX_PROCS; i++) {
                pcb_t *c = sched_get(i);
                if (!c || c->state == PROC_FREE || c->parent_pid != cur) continue;
                if (a != (uint32_t)-1 && i != a) continue;
                have_child = 1;
                break;
            }
            if (!have_child) { r->eax = (uint32_t)-1; return; }
            /* 记录 status 出参指针（唤醒时内核切到本进程地址空间写入） */
            pcb_t *me = sched_get(cur);
            if (me) me->block_arg2 = (uint32_t)status;
            serial_printf("[user] wait pid=%u -> block\n", a);
            sched_block(r, BLOCK_WAIT, a);
            __asm__ volatile ("cli; hlt");           /* 不可达 */
            return;                                  /* 显式返回，抑制 fallthrough 告警 */
        }
    }
    case 23: { /* sys_map_page(addr)：在"当前进程"地址空间映射一张清零物理页。
                * v0.11 每进程地址空间演示：同一虚拟地址在不同进程映射到不同物理页，
                * 互不可见；页面随进程退出回收。要求页对齐且位于用户半区、未映射。 */
        if (a < 0x80000000u || a >= 0xFFC00000u || (a & 0xFFF)) {
            serial_printf("[vm] map_page pid=%u bad addr=%x\n", sched_current_pid(), a);
            r->eax = (uint32_t)-1;
            return;
        }
        if (is_mapped(a)) { r->eax = (uint32_t)-1; return; }   /* 已映射，拒绝重复 */
        /* 记账槽满（map_frames[8] 固定）须在分配前拒绝——否则映射后不记账，
         * 进程退出时该帧永久泄漏（审查 P0-1）。 */
        pcb_t *p_ = sched_get(sched_current_pid());
        if (p_ && p_->map_fcount >= 8) {
            serial_printf("[vm] map_page pid=%u map cap 8 full, reject\n",
                          sched_current_pid());
            r->eax = (uint32_t)-1;
            return;
        }
        uint32_t phys = frame_alloc();
        if (!phys) { r->eax = (uint32_t)-1; return; }
        uint32_t *p = (uint32_t *)phys;
        for (int i = 0; i < 1024; i++) p[i] = 0;               /* 清零 */
        map_page(a, phys, 0x7);                                /* 当前进程地址空间 */
        if (p_) p_->map_frames[p_->map_fcount++] = phys;
        serial_printf("[vm] map_page pid=%u addr=%x phys=%x\n",
                      sched_current_pid(), a, phys);
        r->eax = a;
        return;
    }
    case 24:  /* sys_fork()：复制当前进程。父进程返回子 pid；子进程从调用点继续（返回 0） */
        r->eax = (uint32_t)sched_fork(r);
        return;
    case 25:  /* sys_exec(name, argc, argv)：加载 ELF 替换当前进程（镜像替换）。
                 argv 为用户空间指针数组（argc 个 char*，最多 8 条）。成功不返回；失败返回 -1。
                 L1：大缓冲下沉 sys_exec_case，避免抬高 dispatch 单帧（栈预算总账） */
        sys_exec_case(r, a, b, c);
        return;
    case 26: { /* sys_fs_seek(fd, off)：定位读写位置，返回新位置或 -1。
        * A-2 ② 参数语义：off 按**无符号 uint32 原生**处理（fs pos 即 uint32）。
        * 负偏移经符号位回绕成巨大 uint32 后照存 pos；其"风险"是后续 read/write 落在文件
        * 尾部之外返回失败/0——已由 read/write 的长度/越界哨兵 fail-closed，非内存不安全，
        * 故不 clamp（要验"在文件内"须查 fs_size，超出本 syscall 职责，留注释备忘）。 */
        fs_file_t *fdt = cur_fdt();
        if (a == 0 || a >= FS_FDS_PER_PROC || !fdt[a].used) { r->eax = (uint32_t)-1; return; }
        fdt[a].pos = b;
        serial_printf("[fs] seek fd=%u -> pos=%u\n", a, fdt[a].pos);
        r->eax = fdt[a].pos;
        return;
    }
    case 27: { /* sys_fs_mkdir(path)：建目录（父目录须存在），返回 inode 或 -1 */
        char path[64];
        if (copyin_str((const char *)a, path, sizeof(path)) < 0) { r->eax = (uint32_t)-1; return; }
        int ino = fs_mkdir(fs_device(), path);
        serial_printf("[fs] mkdir '%s' inode=%d\n", path, ino);
        r->eax = (uint32_t)ino;
        return;
    }
    case 28: { /* sys_fs_rmdir(path)：删空目录（非空/非目录返回 -1） */
        char path[64];
        if (copyin_str((const char *)a, path, sizeof(path)) < 0) { r->eax = (uint32_t)-1; return; }
        blockdev_t *bd = fs_device();
        int ino = fs_lookup(bd, path);        /* 解析实际对象（成功删除后供 revoke 悬垂 fd） */
        int rc = fs_rmdir(bd, path);
        serial_printf("[fs] rmdir '%s' rc=%d\n", path, rc);
        /* D4（红队 RBT-2026-014，BUG-068）：删空目录成功即回收指向该 inode 的悬垂 fd */
        if (rc == 0 && ino >= 0) sched_fd_revoke((uint32_t)ino);
        r->eax = (uint32_t)rc;
        return;
    }
    case 29: { /* sys_fs_sync()：把 ramdisk 全量写回真盘（v0.16 持久化）；无盘返回 -1 */
        int rc = storage_sync();
        serial_printf("[fs] sync -> %d\n", rc);
        r->eax = (uint32_t)rc;
        return;
    }
    case 30: { /* sys_net_socket(port)：创建 UDP socket；port=0 自动分配；返回 socket id */
        if (b || c) { r->eax = (uint32_t)-1; return; }
        int s = netsock_open((uint16_t)a);
        if (s < 0) serial_printf("[net] socket table full\n");   /* v0.31 观测：表满专项日志 */
        serial_printf("[net] socket port=%u -> id=%d\n", (uint16_t)a, s);
        r->eax = (uint32_t)s;
        return;
    }
    case 31:  /* sys_net_sendto(sock, *iov)：发 UDP 数据报，返回实际发送字节数或 -1。
                 L1：发送缓冲 pbuf[1400] 下沉 sys_sendto_case（栈预算总账） */
        sys_sendto_case(r, a, b);
        return;
    case 32: { /* sys_net_recvfrom(sock, *iov)：非阻塞收 UDP 数据报（0=无包）；src_* 出参 */
        struct net_recv_iov iov;
        if (copyin((const void *)b, &iov, sizeof(iov)) < 0) { r->eax = (uint32_t)-1; return; }
        if (iov.max > 1400) iov.max = 1400;
        if (iov.max && !user_ptr_valid(iov.buf, iov.max)) { r->eax = (uint32_t)-1; return; }
        int n = netsock_recv((int)a, iov.buf, iov.max, &iov.src_ip, &iov.src_port);
        if (n > 0) copyout(&iov, (void *)b, sizeof(iov));   /* 回写出参 */
        serial_printf("[net] recvfrom sock=%d -> %dB\n", (int)a, n);
        r->eax = (uint32_t)n;
        return;
    }
    case 33: { /* sys_net_close(sock)：v0.31 仅可关自己打开的 socket；内核保留(DHCP)槽不可关 */
        r->eax = (uint32_t)netsock_close_if_owner((int)a, sched_current_pid());
        return;
    }
    case 34: /* sys_kern_audit()：v0.21 内核自审计——帧配平/信号量守恒/PCB 状态机。
                返回失败检查项总数（0=全部通过），细节见 [audit] 日志行 */
        r->eax = kern_audit();
        return;
    case 35: { /* sys_brk(addr)：v0.26#2 用户堆。addr=0 返回当前 brk（sbrk(0) 风格）；
                否则设置 program break，返回 0 成功 / -1 失败。
                向上扩展按页补映射（记账 heap_frames[]）；收缩只更新 brk 保留映射复用。 */
        pcb_t *p = sched_get(sched_current_pid());
        if (!p) { r->eax = (uint32_t)-1; return; }
        if (a == 0) { r->eax = p->heap_brk; return; }
        if (!brk_in_range(a, p->heap_base, USER_HEAP_MAX)) {
            serial_printf("[heap] brk pid=%u bad addr=%x\n", p->pid, a);
            r->eax = (uint32_t)-1;
            return;
        }
        uint32_t old = p->heap_brk;
        if (a > old) {
            /* 容量守卫：目标 top 覆盖的堆页数（自 heap_base 起）须 ≤ 堆区总页数。
             * 不用 brk_pages_up(old,a)（旧 brk→新 brk 的跨度）：收缩后旧映射保留
             * （heap_fcount 不减、帧从不释放），再涨过同一段会重复计数、在真实预算
             * 内误拒；而映射页恒为 [base, top) 前缀，所需新帧数 = 目标页数 - 已记账
             * ≤ 目标页数。单调增长下两式等价；收缩-再涨下本式正确（S8）。 */
            uint32_t cap = ((a - p->heap_base) + 0xFFFu) >> 12;
            if (cap > USER_HEAP_PAGES) {
                serial_printf("[heap] brk pid=%u over cap %u pages\n", p->pid, cap);
                r->eax = (uint32_t)-1;
                return;
            }
            /* v0.26 bugfix: 映射 [old,a) 相交的"所有页"（old 下取整、a 上取整），
             * 否则 brk 落在页中部时顶部半页未映射，任意非页对齐 malloc 都会越界缺页。
             * （heapdemo 用页对齐 sbrk 没暴露；cc500 任意尺寸 malloc 踩中） */
            uint32_t v = old & 0xFFFFF000u;
            uint32_t vend = (a + 0xFFFu) & 0xFFFFF000u;
            while (v < vend) {
                if (!is_mapped(v)) {          /* 已映射页（收缩后复用）跳过 */
                    uint32_t phys = frame_alloc();
                    if (!phys) { r->eax = (uint32_t)-1; return; }
                    uint32_t *wp = (uint32_t *)phys;
                    for (int i = 0; i < 1024; i++) wp[i] = 0;
                    map_page(v, phys, 0x7);   /* 当前进程地址空间 */
                    if (p->heap_fcount < USER_HEAP_PAGES)
                        p->heap_frames[p->heap_fcount++] = phys;
                }
                v += 0x1000u;
            }
        }
        p->heap_brk = a;
        serial_printf("[heap] brk pid=%u %x -> %x pages=%u\n", p->pid, old, a, p->heap_fcount);
        r->eax = 0;
        return;
    }
    case 36: { /* sys_limit(mask_lo, mask_hi)：v0.34 BUG-058 per-process syscall 掩码 · 只收窄。
                 a=mask 低 32 位, b=高 32 位；与当前掩码按位 OR（|= 无清位/放宽路径）。
                 若本进程已禁用位 36，则入口检查会挡住本调用（只能继续收窄）——单向语义自洽。 */
        pcb_t *p = sched_get(sched_current_pid());
        if (p) p->sc_mask |= ((uint64_t)(uint32_t)b << 32) | (uint32_t)a;
        serial_printf("[syscall] pid=%u limit -> mask=%08x%08x\n",
                      sched_current_pid(), (uint32_t)(p ? (uint32_t)(p->sc_mask >> 32) : 0),
                      (uint32_t)(p ? (uint32_t)p->sc_mask : 0));
        r->eax = 0;
        return;
    }
    default:
        serial_printf("[user] unknown syscall %u\n", num);
        r->eax = (uint32_t)-1;
        return;
    }
    (void)b; (void)c;
}
