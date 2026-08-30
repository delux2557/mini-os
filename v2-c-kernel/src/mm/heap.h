/* mini-os/v2-c-kernel/heap.h
 * 内核堆分配器：kmalloc/kfree（首次适配 + 分裂 + 合并） */
#ifndef _HEAP_H
#define _HEAP_H
#include <stdint.h>

void heap_init(void);
void *kmalloc(uint32_t size);  /* 失败返回 NULL */
void kfree(void *ptr);
uint32_t heap_page_count(void); /* 向帧分配器要了多少页 */
uint32_t heap_audit(void);      /* v0.29 堆完整性审计：返回失败检查项数（0=通过） */

#endif
