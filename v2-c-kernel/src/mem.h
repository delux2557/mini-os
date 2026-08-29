/* mini-os/v2-c-kernel/mem.h
 * 内存管理子系统：
 *  - 物理帧分配器（位图管理 4KB 帧，位于 1MB 之上）
 *  - 分页（页目录/页表，内核恒等映射整个物理内存 + 动态映射）
 *  - v0.11 每进程地址空间：每个用户进程独立页目录，切换进程时切 CR3
 *  - 页错误懒分配（demand paging 演示） */
#ifndef _MEM_H
#define _MEM_H
#include <stdint.h>
#include "idt.h"   /* registers_t */

/* 从 multiboot 信息检测内存并初始化帧分配器（保留内核区） */
void mem_init(uint32_t mb_info);
uint32_t total_memory_kb(void);
uint32_t free_memory_kb(void);

/* 分页 */
void paging_init(void);
/* 映射一页到指定页目录（v0.11 每进程地址空间用） */
void map_page_in(uint32_t pd, uint32_t virt, uint32_t phys, uint32_t flags);
/* 映射一页到"当前活动"页目录（读 CR3；启动/懒分配/当前进程用） */
void map_page(uint32_t virt, uint32_t phys, uint32_t flags);
int  is_mapped(uint32_t virt);          /* 检查当前活动页目录 */
uint32_t virt_to_phys(uint32_t virt);
/* 切换当前地址空间（写 CR3，刷新 TLB）；pd=0 视为内核页目录 */
void switch_page_dir(uint32_t pd);
uint32_t mem_current_pd(void);          /* 当前活动页目录物理地址 */
uint32_t mem_kernel_pd(void);           /* 内核页目录物理地址 */

/* ---- v0.11 地址空间生命周期 ---- */
uint32_t addr_space_create(void);       /* 新建页目录：克隆内核共享 PDE + 清零用户区，返回物理地址 */
void addr_space_destroy(uint32_t pd);   /* 释放该页目录独占的页表帧 + 页目录帧 */

/* ---- 用户地址空间布局（v0.12：fork 需识别"共享内存区"以跳过深拷贝） ----
 * 与 usermode.c 的 sys_shmem 严格一致：共享页映射在 [SHMEM_VBASE, +SLOTS*4K)，
 * 所有进程映射到同一物理帧，fork 时保持共享（不深拷贝）。 */
#define SHMEM_VBASE   0x80020000u
#define SHMEM_SLOTS   4

/* 物理帧分配器：返回物理地址（4KB 对齐） */
uint32_t frame_alloc(void);
uint32_t frame_alloc_run(uint32_t count); /* 分配 count 个连续帧 */
void frame_free(uint32_t phys);

/* 页错误处理（ISR 14 路由进来），懒分配区自动补页后可恢复 */
void pf_handler(registers_t *r);
uint32_t lazy_page_count(void);

#endif
