/* mini-os/v2-c-kernel/mem.c
 * 内存管理实现：
 *  1) mem_init  : 从 multiboot 读内存大小，位图标记内核区已用
 *  2) frame_*   : 4KB 物理帧分配器（首次适配，地址位于 1MB 之上）
 *  3) paging    : 建页目录/页表，恒等映射低 16MB，开启 CR0.PG+WP
 *  4) pf_handler: 页错误处理；对"懒分配区"的访问自动补页并重试 */
#include "mem.h"
#include "idt.h"
#include "serial.h"
#include "vga.h"
#include "sched.h"
#include <stdint.h>

#define PAGE_SIZE     4096u
#define MB            (1024u * 1024u)
#define FRAME_ADDR(n) (MB + (uint32_t)(n) * PAGE_SIZE)
#define FRAME_INDEX(a) (((uint32_t)(a) - MB) / PAGE_SIZE)

/* 帧位图上限：最多管理 512MB */
#define MAX_FRAMES    (512u * 1024u * 1024u / PAGE_SIZE)
/* 内核恒等映射范围（够覆盖内核/堆/页表/页目录） */
#define ID_MAP_SIZE   (16u * MB)
/* 懒分配演示区：一段初始未映射的虚拟地址 */
#define LAZY_START    0x40000000u
#define LAZY_END      0x41000000u

extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

static uint8_t bitmap[MAX_FRAMES / 8];
static uint32_t nframes;     /* 1MB 以上的帧总数 */
static uint32_t used_frames; /* 已用帧数 */
static uint32_t total_kb;    /* 总内存 KB */

/* 页目录：4KB 对齐（静态数组放 .bss，位于内核区内） */
static uint32_t page_directory[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t lazy_pages = 0;

static inline void bit_set(uint32_t n)   { bitmap[n >> 3] |=  (uint8_t)(1u << (n & 7)); }
static inline void bit_clear(uint32_t n) { bitmap[n >> 3] &= (uint8_t)~(1u << (n & 7)); }
static inline int  bit_test(uint32_t n)  { return (bitmap[n >> 3] >> (n & 7)) & 1; }

/*--------------------------------------------------------------*/
/* 物理内存检测与初始化                                            */
/*--------------------------------------------------------------*/
void mem_init(uint32_t mb_info) {
    uint32_t mem_upper = 0;

    /* multiboot info 布局：flags@0, mem_lower@4, mem_upper@8 */
    if (mb_info && (*(uint32_t *)mb_info & 1))
        mem_upper = *(uint32_t *)(mb_info + 8);
    if (mem_upper == 0) mem_upper = 15 * 1024;      /* 兜底：16MB */

    total_kb = mem_upper + 1024;                    /* 加上低 1MB */
    if (total_kb > MAX_FRAMES * PAGE_SIZE / 1024)
        total_kb = MAX_FRAMES * PAGE_SIZE / 1024;

    nframes = (total_kb - 1024) / 4;                /* 1MB 以上按 4KB 一帧 */
    for (uint32_t i = 0; i < nframes / 8; i++) bitmap[i] = 0;

    /* 保留内核镜像占用的帧 [1MB, _kernel_end)（含页目录/位图所在 .bss） */
    uint32_t kend = FRAME_INDEX((uint32_t)&_kernel_end + PAGE_SIZE - 1);
    if (kend > nframes) kend = nframes;
    for (uint32_t i = 0; i < kend; i++) { bit_set(i); used_frames++; }
}

uint32_t total_memory_kb(void) { return total_kb; }
uint32_t free_memory_kb(void)  { return (nframes - used_frames) * (PAGE_SIZE / 1024); }

/*--------------------------------------------------------------*/
/* 物理帧分配器                                                    */
/*--------------------------------------------------------------*/
uint32_t frame_alloc(void) {
    for (uint32_t n = 0; n < nframes; n++) {
        if (!bit_test(n)) {
            bit_set(n);
            used_frames++;
            return FRAME_ADDR(n);
        }
    }
    serial_printf("[mem] OUT OF MEMORY (frame_alloc)\n");
    return 0;
}

uint32_t frame_alloc_run(uint32_t count) {
    uint32_t run = 0;
    for (uint32_t n = 0; n < nframes; n++) {
        if (bit_test(n)) { run = 0; continue; }
        if (++run == count) {
            uint32_t start = n + 1 - count;
            for (uint32_t k = start; k <= n; k++) { bit_set(k); used_frames++; }
            return FRAME_ADDR(start);
        }
    }
    serial_printf("[mem] OUT OF MEMORY (frame_alloc_run %u)\n", count);
    return 0;
}

void frame_free(uint32_t phys) {
    if (phys < MB) return;
    uint32_t n = FRAME_INDEX(phys);
    if (n >= nframes) return;
    if (!bit_test(n)) {
        serial_printf("[mem] double free of frame %x\n", phys);
        return;
    }
    bit_clear(n);
    used_frames--;
}

/* v0.21 内核自审计：验证 used_frames 与帧位图实际置位数配平。
 * 任何 alloc/free 记账不一致（漏记、重复释放、计数漂移）都会在此暴露。
 * 返回失败检查项数（0=全部通过），并打印一行 [audit] 结果。 */
uint32_t mem_audit(void) {
    uint32_t bad = 0;
    uint32_t bcount = 0;
    for (uint32_t i = 0; i < nframes; i++) bcount += bit_test(i);
    if (used_frames != bcount) {
        serial_printf("[audit] mem FAIL: used=%u bitmap=%u\n", used_frames, bcount);
        vga_printf("[audit] mem FAIL: used=%u bitmap=%u\n", used_frames, bcount);
        bad++;
    }
    if (used_frames > nframes) {
        serial_printf("[audit] mem FAIL: used_frames=%u > nframes=%u\n",
                      used_frames, nframes);
        bad++;
    }
    if (bad == 0)
        serial_printf("[audit] mem ok: frames used=%u/%u\n", used_frames, nframes);
    return bad;
}

/*--------------------------------------------------------------*/
/* 分页                                                            */
/*--------------------------------------------------------------*/
void map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    /* v0.11: 一律映射到"当前活动"页目录（读 CR3）。
     * 用户进程在运行/系统调用期间，CR3 即其私有页目录，故映射进它自己的地址空间；
     * 内核上下文 CR3 为内核页目录。 */
    map_page_in(mem_current_pd(), virt, phys, flags);
}

/* v0.11：把一页映射到"指定页目录"（每进程地址空间用）。
 * pd 为目标页目录物理地址；页表帧/页目录帧都落在低 16MB 恒等映射区，
 * 当前地址空间（无论哪个进程）都能直接写入，故可在任意上下文安全调用。 */
void map_page_in(uint32_t pd, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;
    uint32_t *dir = (uint32_t *)pd;

    if (!(dir[pd_idx] & 1)) {
        uint32_t pt_phys = frame_alloc();
        uint32_t *pt = (uint32_t *)pt_phys;
        for (int i = 0; i < 1024; i++) pt[i] = 0;
        dir[pd_idx] = (pt_phys & ~(PAGE_SIZE - 1)) | (flags & 0xFFF);
    }
    uint32_t *pt = (uint32_t *)(dir[pd_idx] & ~(PAGE_SIZE - 1));
    pt[pt_idx] = (phys & ~(PAGE_SIZE - 1)) | (flags & 0xFFF);
}

/* 切换当前地址空间：写 CR3（写 CR3 自动刷新 TLB）；pd=0 视为内核页目录 */
void switch_page_dir(uint32_t pd) {
    uint32_t cr3 = pd ? pd : (uint32_t)page_directory;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uint32_t mem_current_pd(void) {
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

uint32_t mem_kernel_pd(void) { return (uint32_t)page_directory; }

/* ---- v0.11 地址空间生命周期 ----
 * 新建：克隆内核共享 PDE（低 1GB：恒等映射低 16MB + 懒分配区），清空用户半区。
 * 用户半区（>=2GB，PDE>=512）的页表为进程独占；内核半区页表与内核页目录共享。 */
uint32_t addr_space_create(void) {
    uint32_t pd = frame_alloc();
    if (!pd) return 0;
    uint32_t *dir = (uint32_t *)pd;
    for (int i = 0; i < 512; i++) dir[i] = page_directory[i];
    for (int i = 512; i < 1024; i++) dir[i] = 0;
    return pd;
}

/* 销毁：释放该地址空间独占的用户半区页表帧 + 页目录帧。
 * 注意：数据页（用户栈/ELF/共享内存）由调度器或全局持有方单独回收，
 * 这里只回收"页表/页目录"这种进程独占的管理帧，避免双重释放。 */
void addr_space_destroy(uint32_t pd) {
    if (!pd) return;
    uint32_t *dir = (uint32_t *)pd;
    for (int i = 512; i < 1024; i++)
        if (dir[i] & 1) frame_free(dir[i] & ~(PAGE_SIZE - 1));
    frame_free(pd);
}

int is_mapped(uint32_t virt) {
    uint32_t pd = mem_current_pd();   /* v0.11: 检查当前活动地址空间 */
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;
    uint32_t *dir = (uint32_t *)pd;
    if (!(dir[pd_idx] & 1)) return 0;
    uint32_t *pt = (uint32_t *)(dir[pd_idx] & ~(PAGE_SIZE - 1));
    return (pt[pt_idx] & 1) != 0;
}

uint32_t virt_to_phys(uint32_t virt) {
    uint32_t pd = mem_current_pd();   /* v0.11: 查当前活动地址空间 */
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;
    uint32_t *dir = (uint32_t *)pd;
    if (!(dir[pd_idx] & 1)) return 0;
    uint32_t *pt = (uint32_t *)(dir[pd_idx] & ~(PAGE_SIZE - 1));
    if (!(pt[pt_idx] & 1)) return 0;
    return (pt[pt_idx] & ~(PAGE_SIZE - 1)) | (virt & (PAGE_SIZE - 1));
}

void paging_init(void) {
    for (int i = 0; i < 1024; i++) page_directory[i] = 0;

    /* v0.11: map_page 现按"当前 CR3"定位页目录，故须先把 CR3 指向内核页目录
     * 再建映射（此时 CR0.PG 未开，写 CR3 只是保存值，安全）。
     * 恒等映射低 16MB：内核、显存、页表、堆、懒分配页表都在这 */
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)page_directory) : "memory");
    for (uint32_t a = 0; a < ID_MAP_SIZE; a += PAGE_SIZE)
        map_page(a, a, 0x3);

    /* CR0: 置位 PG(0x80000000) 与 WP(0x10000) */
    __asm__ volatile ("mov %%cr0, %%eax; orl $0x80010000, %%eax; mov %%eax, %%cr0"
                      ::: "eax", "memory");
}

/*--------------------------------------------------------------*/
/* 页错误处理（ISR 14）                                            */
/*--------------------------------------------------------------*/
void pf_handler(registers_t *r) {
    uint32_t fault = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault));

    /* 用户态页错误：用户程序越界访问（内核内存/非法地址）-> 隔离终止该进程 */
    if ((r->cs & 3) == 3) {
        uint32_t pid = sched_current_pid();
        pcb_t *p = sched_get(pid);
        stack_evt_t ev = stack_guard_hit(fault, pid, p ? p->stack_bottom : 0);

        /* v0.26 栈按需生长：命中"当前守卫页"（栈底下方 1 页）且槽内还有空间
         * -> 补映射一页、守卫页随之下移，iret 重试触发异常的指令 */
        if (ev == STACK_GROWTH) {
            if (p && p->stack_fcount < USER_STACK_PAGES) {
                uint32_t newb = p->stack_bottom - PAGE_SIZE;
                uint32_t phys = frame_alloc();
                if (phys) {
                    map_page_in(p->page_dir, newb, phys, 0x7);
                    p->stack_frames[p->stack_fcount++] = phys;
                    p->stack_bottom = newb;
                    serial_printf("[stack] grow pid=%u @%x pages=%u\n",
                                  pid, newb, p->stack_fcount);
                    vga_printf("[stack] grow pid=%u @%x pages=%u\n",
                               pid, newb, p->stack_fcount);
                    return;   /* iret 后自动重试触发异常的指令 */
                }
            }
            ev = STACK_BOOM;   /* 无物理帧 / 已生长到上限 / 无 PCB：视同栈溢出 */
        }

        /* v0.13 栈溢出检测：fault 深越界（越过当前守卫页，或已到槽底硬底） */
        if (ev == STACK_BOOM) {
            serial_printf("\n[user] STACK OVERFLOW pid=%u @%x -> killed\n", pid, fault);
            vga_printf("\n[user] STACK OVERFLOW pid=%u @%x -> killed\n", pid, fault);
            sched_kill(r, (uint32_t)-1);
            __asm__ volatile ("cli; hlt");   /* 不可达 */
        }

        serial_printf("\n[user] PAGE FAULT pid=%u @%x err=%u -> killed\n",
                      pid, fault, r->err_code);
        vga_printf("\n[user] PAGE FAULT pid=%u @%x err=%u -> killed\n",
                   pid, fault, r->err_code);
        sched_kill(r, (uint32_t)-1);
        __asm__ volatile ("cli; hlt");   /* 不可达 */
    }

    if (fault >= LAZY_START && fault < LAZY_END) {
        /* 懒分配：访问未映射页 -> 补一张物理页再重试 */
        uint32_t page = fault & ~(PAGE_SIZE - 1);
        if (!is_mapped(page)) {
            uint32_t phys = frame_alloc();
            if (!phys) {
                serial_printf("[lazy] OOM\n");
                __asm__ volatile ("cli; hlt");
            }
            map_page(page, phys, 0x3);
            lazy_pages++;
            serial_printf("[lazy] PF@%x -> phys %x (#%u)\n", page, phys, lazy_pages);
            vga_printf("[lazy] PF@%x -> phys %x (#%u)\n", page, phys, lazy_pages);
        }
        return;   /* iret 后自动重试触发异常的指令 */
    }

    /* 其它页错误：不可恢复，停机 */
    serial_printf("\n[FATAL] page fault @%x err=%u eip=%x\n",
                  fault, r->err_code, r->eip);
    vga_printf("\n[FATAL] page fault @%x err=%u eip=%x\n",
               fault, r->err_code, r->eip);
    __asm__ volatile ("cli; hlt");
}

uint32_t lazy_page_count(void) { return lazy_pages; }

/* 用户栈守卫页判定（v0.13）实现在 src/guard.c（纯逻辑，可宿主单测）；
 * 声明见 mem.h，此处由 pf_handler 使用。 */
