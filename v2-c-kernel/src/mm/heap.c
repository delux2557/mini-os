/* mini-os/v2-c-kernel/heap.c
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

/* 从帧分配器要 npages 张连续物理页，作为一个大空闲块挂入链表 */
static void heap_add_pages(uint32_t npages) {
    if (npages == 0) npages = 1;
    uint32_t phys = frame_alloc_run(npages);
    if (!phys) {
        serial_printf("[heap] OOM (heap_add_pages %u)\n", npages);
        return;
    }
    block_t *b = (block_t *)phys;
    b->next  = head;
    b->size  = npages * PAGE_SIZE - HDR_SIZE;
    b->free  = 1;
    b->magic = MAGIC_FREE;
    head = b;
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

void *kmalloc(uint32_t size) {
    if (size == 0) size = 1;
    size = (size + 7u) & ~7u;             /* 8 字节对齐 */

    for (block_t *b = head; b; b = b->next)
        if (b->free && b->size >= size)
            return block_claim(b, size);

    /* 不够则按需补连续页再试一次 */
    uint32_t need = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    heap_add_pages(need);
    for (block_t *b = head; b; b = b->next)
        if (b->free && b->size >= size)
            return block_claim(b, size);

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
     * 注：每次合并后从头重扫，最坏 O(n²)；仅适用于小块数场景（当前内核堆 <20 块）。
     * 若未来引入 mmap/COW 等复杂内存管理，需改为按地址序链表或二叉堆优化。 */
    int merged;
    do {
        merged = 0;
        for (block_t *o = head; o; o = o->next) {
            if (o == b || !o->free) continue;
            block_t *low = b, *high = o;
            if ((uint32_t)o < (uint32_t)b) { low = o; high = b; }
            if ((uint32_t)high == (uint32_t)low + HDR_SIZE + low->size) {
                low->size += HDR_SIZE + high->size;
                low->next  = high->next;
                b = low;
                free_bytes += HDR_SIZE;  /* 两块之间的头被合并回收，变为可用负载 */
                merged = 1;
                break;
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
    while (b) {
        if (++blocks > max_blocks) {
            serial_printf("[audit] heap FAIL: next chain loop/suspect (walk>%u)\n", max_blocks);
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
