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
#include "elf.h"
#include "kb.h"
#include "storage.h"
#include "userptr.h"
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

/* ---- v0.8 文件系统：内核打开文件表（用户通过固定槽位引用，槽 0 保留） ---- */
#define FS_MAX_OBJ 8
typedef struct {
    int      used;
    uint32_t inode;
    uint32_t pos;      /* 当前读写位置 */
    uint32_t mode;     /* 0=只读 1=只写 */
} fs_file_t;
static fs_file_t fs_files[FS_MAX_OBJ];

/* ---- v0.9 从文件系统加载 ELF 应用 ----
 * 固定 app 槽：hello/echo/crash 等链接到 APP_LINK（0x80040000，16KB），
 * 加载时用 mapfn 逐页分配物理帧并映射；退出时由调度器回收（pcb.own_frames）。
 * shell 常驻 SHELL_LINK（0x80030000），帧不随退出回收。 */
#define APP_LINK      0x80040000u
#define APP_REGION    0x4000u        /* 16KB = 4 页（按需扩张的大上限） */
#define APP_MAXFRAMES 8

static uint32_t load_vbase;        /* mapfn 边界检查：ELF 实际最低链接地址（页对齐） */
static uint32_t load_region;       /* 覆盖范围字节数 = load_maxframes 页 */
static uint32_t load_frames[APP_MAXFRAMES];   /* 各页的物理帧 */
static uint32_t load_fcount;
static uint32_t load_maxframes;
static uint32_t load_pd;           /* v0.11: 本次加载的目标进程页目录 */
static int      load_failed;

/* elf_load 的映射钩子：为目标虚拟区分配物理帧并映射进"目标进程页目录"。
 * v0.11: 加载期间 CR3 已被切到目标页目录，故只映射进 load_pd 即可，
 * 段数据由 elf_load 直接写入目标地址空间，不再污染当前（父进程）页目录。
 * 若父进程与子进程链接到同一虚拟地址，旧方案（临时映射进当前页目录）会
 * 覆盖父进程自身映射导致其恢复运行时缺页。 */
static void app_mapfn(uint32_t vaddr, uint32_t len) {
    if (load_failed) return;
    uint32_t start = vaddr & 0xFFFFF000u;
    uint32_t end = (vaddr + len + 0xFFFu) & 0xFFFFF000u;
    if (start < load_vbase || end > load_vbase + load_region) { load_failed = 1; return; }
    for (uint32_t pg = start; pg < end; pg += 0x1000) {
        if (load_fcount >= load_maxframes) { load_failed = 1; return; }
        uint32_t phys = frame_alloc();
        if (!phys) { load_failed = 1; return; }
        map_page_in(load_pd, pg, phys, 0x7);   /* 目标进程地址空间（P|RW|U） */
        load_frames[load_fcount] = phys;
        load_fcount++;
    }
}

/* 从文件系统读取 ELF 并加载到指定区域，成功返回 0 并把入口写入 *entry */
static int load_elf_file(const char *name, uint32_t vbase, uint32_t *entry) {
    blockdev_t *bd = fs_device();
    int ino = fs_lookup(bd, name);
    if (ino < 0) { serial_printf("[elf] '%s' not found\n", name); return -1; }
    uint32_t sz = fs_size(bd, (uint32_t)ino);
    if (sz == 0 || sz > 65536) { serial_printf("[elf] '%s' bad size %u\n", name, sz); return -1; }

    uint8_t *buf = (uint8_t *)kmalloc(sz);
    if (!buf) return -1;
    if (fs_read(bd, (uint32_t)ino, buf, 0, sz) != (int)sz) { kfree(buf); return -1; }

    /* 映射区间 = ELF 自带 PT_LOAD 范围（页对齐），vbase 仅用于日志/登记。
     * 注意 ELF 头页在 -Ttext 地址的前一页，region 必须含它，否则 mapfn 拒绝、拷贝缺页。 */
    uint32_t lbase, lend;
    if (elf_load_range(buf, sz, &lbase, &lend) != 0) { kfree(buf); return -1; }
    load_vbase = lbase;
    load_region = lend - lbase;
    load_maxframes = load_region / 0x1000u;
    if (load_maxframes > APP_MAXFRAMES) {
        serial_printf("[elf] '%s' too big (%u pages > %u)\n", name,
                      load_maxframes, APP_MAXFRAMES);
        kfree(buf);
        return -1;
    }
    load_fcount = 0;
    load_failed = 0;
    (void)vbase;
    int rc = elf_load(buf, sz, 0, app_mapfn, entry);   /* 0=按链接地址原样放置 */
    kfree(buf);
    if (rc != 0 || load_failed) {
        for (uint32_t i = 0; i < load_fcount; i++) frame_free(load_frames[i]);
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
     * 之后所有日志/拷贝都用这份内核内存中的名字。 */
    char namebuf[16];
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
        addr_space_destroy(pd);
        serial_printf("[elf] spawn '%s' failed (no pid)\n", namebuf);
        load_fcount = 0;
        return -1;
    }
    /* v0.11: 帧仅映射在目标进程页目录（进程运行时使用），当前地址空间无临时映射，
     * 无需清理；帧已移交 pcb（成功时）/已回收（失败时） */
    load_fcount = 0;
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

/* ---- 系统调用分发（int 0x80 门进入） ---- */
void syscall_dispatch(registers_t *r) {
    /* 只允许来自用户态（ring3）的触发 */
    if ((r->cs & 3) != 3) {
        serial_printf("[syscall] rejected from ring %u\n", r->cs & 3);
        r->eax = (uint32_t)-1;
        return;
    }
    uint32_t num = r->eax;
    uint32_t a = r->ebx, b = r->ecx, c = r->edx;

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
    case 14: { /* sys_fs_open(slot, name, mode)：mode 0=只读 1=只写 2=追加(v0.14) */
        if (a == 0 || a >= FS_MAX_OBJ || fs_files[a].used) { r->eax = (uint32_t)-1; return; }
        char path[64];
        if (copyin_str((const char *)b, path, sizeof(path)) < 0) { r->eax = (uint32_t)-1; return; }
        int ino = fs_lookup(fs_device(), path);
        if (ino < 0) { r->eax = (uint32_t)-1; return; }
        fs_files[a].used  = 1;
        fs_files[a].inode = (uint32_t)ino;
        fs_files[a].pos   = (c == 2) ? fs_size(fs_device(), (uint32_t)ino) : 0;
        fs_files[a].mode  = c;
        serial_printf("[fs] open slot=%u '%s' inode=%u mode=%u pos=%u\n",
                      a, path, ino, c, fs_files[a].pos);
        r->eax = 0;
        return;
    }
    case 15: { /* sys_fs_write(slot, buf, len)：从当前位置写（buf 须为合法用户指针） */
        if (a == 0 || a >= FS_MAX_OBJ || !fs_files[a].used) { r->eax = (uint32_t)-1; return; }
        fs_file_t *f = &fs_files[a];
        if (f->mode == 0) { r->eax = (uint32_t)-1; return; }   /* 只读槽不可写 */
        if (!user_ptr_valid((const void *)b, c)) { r->eax = (uint32_t)-1; return; }
        int n = fs_write(fs_device(), f->inode, (const void *)b, f->pos, c);
        if (n > 0) f->pos += (uint32_t)n;
        serial_printf("[fs] write slot=%u inode=%u pos=%u +%d\n", a, f->inode, f->pos, n);
        r->eax = (uint32_t)n;
        return;
    }
    case 16: { /* sys_fs_read(slot, buf, len)：从当前位置读（buf 须为合法用户指针） */
        if (a == 0 || a >= FS_MAX_OBJ || !fs_files[a].used) { r->eax = (uint32_t)-1; return; }
        fs_file_t *f = &fs_files[a];
        if (f->mode != 0) { r->eax = (uint32_t)-1; return; }
        if (!user_ptr_valid((const void *)b, c)) { r->eax = (uint32_t)-1; return; }
        int n = fs_read(fs_device(), f->inode, (void *)b, f->pos, c);
        if (n > 0) f->pos += (uint32_t)n;
        serial_printf("[fs] read slot=%u inode=%u pos=%u +%d\n", a, f->inode, f->pos, n);
        r->eax = (uint32_t)n;
        return;
    }
    case 17: { /* sys_fs_close(slot) */
        if (a == 0 || a >= FS_MAX_OBJ || !fs_files[a].used) { r->eax = (uint32_t)-1; return; }
        fs_files[a].used = 0;
        serial_printf("[fs] close slot=%u\n", a);
        r->eax = 0;
        return;
    }
    case 18: { /* sys_fs_ls(path)：列出目录内容（v0.14 路径化；path 空/0=根目录） */
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
        return;
    }
    case 19: { /* sys_fs_delete(name)：删除文件（v0.14 支持路径；目录用 sys_fs_rmdir） */
        char path[64];
        if (copyin_str((const char *)a, path, sizeof(path)) < 0) { r->eax = (uint32_t)-1; return; }
        int rc = fs_delete(fs_device(), path);
        serial_printf("[fs] delete '%s' rc=%d\n", path, rc);
        r->eax = (uint32_t)rc;
        return;
    }
    case 20: { /* sys_readline(buf, max)：阻塞式读一行；行已就绪则直接返回长度 */
        char *out = (char *)a;
        uint32_t max = b ? b : KB_LINE_MAX + 1;
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
        char namebuf[16];
        if (copyin_str((const char *)a, namebuf, sizeof(namebuf)) < 0) { r->eax = (uint32_t)-1; return; }
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
        uint32_t phys = frame_alloc();
        if (!phys) { r->eax = (uint32_t)-1; return; }
        uint32_t *p = (uint32_t *)phys;
        for (int i = 0; i < 1024; i++) p[i] = 0;               /* 清零 */
        map_page(a, phys, 0x7);                                /* 当前进程地址空间 */
        pcb_t *p_ = sched_get(sched_current_pid());
        if (p_ && p_->map_fcount < 8) p_->map_frames[p_->map_fcount++] = phys;
        serial_printf("[vm] map_page pid=%u addr=%x phys=%x\n",
                      sched_current_pid(), a, phys);
        r->eax = a;
        return;
    }
    case 24:  /* sys_fork()：复制当前进程。父进程返回子 pid；子进程从调用点继续（返回 0） */
        r->eax = (uint32_t)sched_fork(r);
        return;
    case 25: { /* sys_exec(name, argc, argv)：加载 ELF 替换当前进程（镜像替换）。
                  argv 为用户空间指针数组（argc 个 char*，最多 8 条）。成功不返回；失败返回 -1 */
        if (b > 8) { r->eax = (uint32_t)-1; return; }
        uint32_t argc = b;
        char *const *argv = (char *const *)c;
        /* v0.17：name / argv 数组 / 每个 argv[i] 字符串都先校验并拷入内核缓冲。
         * name 与 argv 内容都位于当前（旧）地址空间，而加载/替换期间会切 CR3，
         * 故须先在当前地址空间把它们全部拷入内核缓冲（与 spawn_elf 的 namebuf 同因）。 */
        char namebuf[16];
        if (copyin_str((const char *)a, namebuf, sizeof(namebuf)) < 0) { r->eax = (uint32_t)-1; return; }
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
            addr_space_destroy(pd);
            load_fcount = 0;
            __asm__ volatile ("sti");
            serial_printf("[user] exec '%s' failed (no stack)\n", namebuf);
            r->eax = (uint32_t)-1;
            return;
        }
        load_fcount = 0;
        __asm__ volatile ("cli; hlt");   /* 不可达 */
        return;
    }
    case 26: { /* sys_fs_seek(slot, off)：定位读写位置，返回新位置或 -1 */
        if (a == 0 || a >= FS_MAX_OBJ || !fs_files[a].used) { r->eax = (uint32_t)-1; return; }
        fs_files[a].pos = b;
        serial_printf("[fs] seek slot=%u -> pos=%u\n", a, fs_files[a].pos);
        r->eax = fs_files[a].pos;
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
        int rc = fs_rmdir(fs_device(), path);
        serial_printf("[fs] rmdir '%s' rc=%d\n", path, rc);
        r->eax = (uint32_t)rc;
        return;
    }
    case 29: { /* sys_fs_sync()：把 ramdisk 全量写回真盘（v0.16 持久化）；无盘返回 -1 */
        int rc = storage_sync();
        serial_printf("[fs] sync -> %d\n", rc);
        r->eax = (uint32_t)rc;
        return;
    }
    default:
        serial_printf("[user] unknown syscall %u\n", num);
        r->eax = (uint32_t)-1;
        return;
    }
    (void)b; (void)c;
}
