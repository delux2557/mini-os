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

void heap_init(void) { head = 0; page_count = 0; }

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
}

/* 从空闲块中切出 size 字节（必要时分裂），并标记为已用 */
static void *block_claim(block_t *b, uint32_t size) {
    if (b->size >= size + HDR_SIZE + 8) {
        block_t *n = (block_t *)((char *)(b + 1) + size);
        n->next  = b->next;
        n->size  = b->size - size - HDR_SIZE;
        n->free  = 1;
        n->magic = MAGIC_FREE;
        b->size  = size;
        b->next  = n;
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
    b->free  = 1;
    b->magic = MAGIC_FREE;

    /* 与相邻空闲块合并（循环直到无可合并） */
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
                merged = 1;
                break;
            }
        }
    } while (merged);
}

uint32_t heap_page_count(void) { return page_count; }
