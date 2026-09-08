/* mini-os/v2-c-kernel/src/mm/heap.c
 * 内核堆分配器：
 *   - 空闲块链表 + 首次适配，支持分裂与相邻合并
 *   - 内存不足时向物理帧分配器申请连续页（v0.9 起支持跨多页的大块，
 *     用 frame_alloc_run 申请连续物理帧，单次 kmalloc 可容纳整份 ELF） */
#include "heap.h"
#include "mem.h"
#include "serial.h"
#include <stdint.h>

#define PAGE_SIZE 4096u
#define HDR_SIZE  16u                     /* block_t 固定头部大小 */
#define MAGIC_FREE 0x5EEDu
#define MAGIC_USED 0xDEADu

typedef struct block {
    struct block *next;
    uint32_t size;                        /* 可用负载字节数 */
    uint32_t free;
    uint32_t magic;
} block_t;

static block_t *head;
static uint32_t page_count;

/* v0.29 记账计数器：已用/空闲负载字节总数（不含块头）。
 * 与链表遍历统计对账，供 heap_audit 做泄漏/双重释放/游离块检测。 */
static uint32_t used_bytes;
static uint32_t free_bytes;

void heap_init(void) { head = 0; page_count = 0; used_bytes = 0; free_bytes = 0; }

/* 扫描兜底：链表成环时（#132 排查中）限步数并报告，防 kmalloc/插入自身死循环 */
static uint32_t heap_scan_limit(void) { return page_count * PAGE_SIZE / 24u + 4u; }

/* 从帧分配器要 npages 张连续物理页，作为一个大空闲块挂入链表。
 * L3（BUG-074 结构根治）：链表按**地址严格递增**插入——使「物理相邻 ⇔ 链表相邻」
 * 成为结构不变量：物理相邻的两块在链表中必然相邻且顺序正确，合并任何写法（含旧的
 * 全链表扫描物理相邻）都不可能成环。插入 O(n)（内核堆 ~20 块，可忽略）。 */
static void heap_add_pages(uint32_t npages) {
    if (npages == 0) npages = 1;
    uint32_t phys = frame_alloc_run(npages);
    if (!phys) {
        serial_printf("[heap] OOM (heap_add_pages %u)\n", npages);
        return;
    }
    block_t *b = (block_t *)phys;
    b->size  = npages * PAGE_SIZE - HDR_SIZE;
    b->free  = 1;
    b->magic = MAGIC_FREE;
    /* 找第一个地址 > b 的节点，插它前面；找不到（b 最大）则挂尾部。
     * 防环上限同 heap_audit/kfree：链表被破坏成环时不至于死循环。 */
    uint32_t lim = heap_scan_limit(), walk = 0;
    block_t *p = 0, *o = head;
    while (o && (uint32_t)o < (uint32_t)b) {
        if (++walk > lim) break;
        p = o;
        o = o->next;
    }
    if (p) { b->next = p->next; p->next = b; }
    else   { b->next = head;    head    = b; }
    page_count += npages;
    free_bytes += npages * PAGE_SIZE - HDR_SIZE;   /* 新空闲块负载入账 */
}

/* 从空闲块中切出 size 字节（必要时分裂），并标记为已用 */
static void *block_claim(block_t *b, uint32_t size) {
    uint32_t old = b->size;
    if (old >= size + HDR_SIZE + 8) {
        block_t *n = (block_t *)((char *)(b + 1) + size);
        n->next  = b->next;
        n->size  = old - size - HDR_SIZE;
        n->free  = 1;
        n->magic = MAGIC_FREE;
        b->size  = size;
        b->next  = n;
        free_bytes -= size + HDR_SIZE;   /* 空闲负载：整块 old → 剩余 old-size-HDR（头被新块占用） */
        used_bytes += size;
    } else {
        free_bytes -= old;               /* 不分裂：整块容量转入已用（含未用余量） */
        used_bytes += old;
    }
    b->free  = 0;
    b->magic = MAGIC_USED;
    return (void *)(b + 1);
}

static void heap_report_loop(const char *where, uint32_t walk) {
    serial_printf("[heap] LOOP at %s (walk>%u) head=%x\n", where, walk, (uint32_t)head);
    block_t *w = head;
    uint32_t n = 0;
    while (w && n < 5) {
        serial_printf("  blk %x next=%x size=%u magic=%x free=%u\n",
                      (uint32_t)w, (uint32_t)w->next, w->size, w->magic, w->free);
        w = w->next; n++;
    }
}

void *kmalloc(uint32_t size) {
    if (size == 0) size = 1;
    size = (size + 7u) & ~7u;             /* 8 字节对齐 */

    uint32_t lim = heap_scan_limit(), walk;
    walk = 0;
    for (block_t *b = head; b; b = b->next) {
        if (++walk > lim) { heap_report_loop("kmalloc pass1", walk); break; }
        if (b->free && b->size >= size)
            return block_claim(b, size);
    }

    /* 不够则按需补连续页再试一次。
     * BUG-056/审计：need 须含 16B 块头——否则请求恰为 N*4096 时新块
     * (N*4096-16 < size) 恒分配失败（可用内存却 OOM）。 */
    uint32_t need = (size + HDR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    heap_add_pages(need);
    walk = 0;
    for (block_t *b = head; b; b = b->next) {
        if (++walk > lim) { heap_report_loop("kmalloc pass2", walk); break; }
        if (b->free && b->size >= size)
            return block_claim(b, size);
    }

    serial_printf("[heap] kmalloc(%u) FAILED\n", size);
    return 0;
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)ptr - 1;
    if (b->magic != MAGIC_USED) {
        serial_printf("[heap] kfree invalid ptr %x\n", (uint32_t)ptr);
        return;
    }
    used_bytes -= b->size;               /* 该块负载容量归还空闲 */
    free_bytes += b->size;
    b->free  = 1;
    b->magic = MAGIC_FREE;

    /* 与相邻空闲块合并（循环直到无可合并）。
     * 注：#132 排查确认原实现有环缺陷——合并条件按"物理地址相邻"（high == low+16+size）
     * 找全链表扫描，但空闲链表**不按地址排序**（heap_add_pages 新块插 head、block_claim
     * 分裂插中间），物理相邻的 two blocks 在链表中可能顺序相反（high 在 low 之前）。
     * 此时 `low->next = high->next` 会把 low 接回 high 之后（更早的链位）→ 环：
     *   merge low=5c2100(sz48880)->+high=5ce000(sz49136) => 98032 后紧接 chain loop。
     * 修复：只与**链表相邻**的空闲块合并（b 的后继/前驱），物理相邻但链表不相邻的块
     * 不合并（保留碎片）——正确性优先，内核堆仅 ~20 块，碎片代价可忽略。
     * 防环上限（heap_audit 同口径）仍保留，任何异常不再升级为 cli 段整机冻结。
     * L3（BUG-074）：heap_add_pages 已按地址序插入后，链表"物理相邻 ⇔ 链表相邻"
     * 恒成立——本逻辑恰好合并**全部**物理相邻空闲块，碎片也一并消除。 */
    int merged;
    uint32_t max_blocks = page_count * PAGE_SIZE / 24u + 4u;   /* 同 heap_audit 防环上界 */
    uint32_t walk = 0;
    do {
        merged = 0;

        /* 1) 与后继合并（b 在前，b->next 紧随 b 之后且空闲） */
        if (b->next && b->next->free &&
            (uint32_t)b->next == (uint32_t)b + HDR_SIZE + b->size) {
            b->size += HDR_SIZE + b->next->size;
            b->next = b->next->next;
            free_bytes += HDR_SIZE;   /* 中间块头被合并回收，变为可用负载 */
            merged = 1;
            continue;
        }

        /* 2) 与前驱合并（prev 在 b 之前，b 紧随 prev 之后且 prev 空闲） */
        {
            block_t *prev = 0;
            walk = 0;
            for (block_t *o = head; o && o != b; o = o->next) {
                if (++walk > max_blocks) break;
                prev = o;
            }
            if (prev && prev->free &&
                (uint32_t)b == (uint32_t)prev + HDR_SIZE + prev->size) {
                prev->size += HDR_SIZE + b->size;
                prev->next = b->next;
                free_bytes += HDR_SIZE;   /* 中间块头被合并回收，变为可用负载 */
                b = prev;
                merged = 1;
            }
        }
    } while (merged);
}

uint32_t heap_page_count(void) { return page_count; }

/* v0.29 堆完整性审计（供 kern_audit 调用）：
 *  - 遍历 block_t 链表：校验 magic/free 一致性、size 上界，防 next 指针成环/悬垂
 *    （超过块数上界即判为成环，立即停止，避免死循环）
 *  - 对账：遍历统计的已用/空闲负载字节 vs 记账计数器 —— 泄漏（块游离于计数外）、
 *    双重释放（计数提前减）、写越界破坏头部的块都会在此暴露
 *  - 碎片报告：空闲块数 + 空闲字节（教学观察用）
 * 返回失败检查项数（0=全部通过）。 */
uint32_t heap_audit(void) {
    uint32_t bad = 0;
    uint32_t blocks = 0, free_sum = 0, used_sum = 0, free_cnt = 0;
    /* 每块至少 8 字节负载 + 16 字节头，块数不可能超过 总字节/24+4 */
    uint32_t max_blocks = page_count * PAGE_SIZE / 24u + 4u;
    block_t *b = head;
    block_t *prev = 0;
    while (b) {
        if (++blocks > max_blocks) {
            serial_printf("[audit] heap FAIL: next chain loop/suspect (walk>%u)\n", max_blocks);
            bad++;
            break;
        }
        /* L3（BUG-074）：地址序结构不变量——链表必须按地址严格递增。
         * 任何破坏排序的插入（回退旧 heap_add_pages 之类）在此当场暴露。 */
        if (prev && (uint32_t)b <= (uint32_t)prev) {
            serial_printf("[audit] heap FAIL: addr order violated @%x after %x\n",
                          (uint32_t)b, (uint32_t)prev);
            bad++;
            break;
        }
        uint32_t m = b->magic;
        if (m != MAGIC_FREE && m != MAGIC_USED) {
            serial_printf("[audit] heap FAIL: bad magic %x @%x\n", m, (uint32_t)b);
            bad++;
            break;
        }
        if ((m == MAGIC_FREE) != (b->free != 0)) {
            serial_printf("[audit] heap FAIL: magic/free mismatch @%x\n", (uint32_t)b);
            bad++;
            break;
        }
        if (b->size > page_count * PAGE_SIZE) {
            serial_printf("[audit] heap FAIL: size %u out of range @%x\n", b->size, (uint32_t)b);
            bad++;
            break;
        }
        if (b->free) { free_sum += b->size; free_cnt++; }
        else         { used_sum += b->size; }
        prev = b;
        b = b->next;
    }
    if (!bad && (free_sum != free_bytes || used_sum != used_bytes)) {
        serial_printf("[audit] heap FAIL: accounting drift free %u!=%u used %u!=%u\n",
                      free_sum, free_bytes, used_sum, used_bytes);
        bad++;
    }
    if (!bad)
        serial_printf("[audit] heap ok: %u blocks, free %u blocks/%uB used %uB pages=%u\n",
                      blocks, free_cnt, free_sum, used_sum, page_count);
    return bad;
}
