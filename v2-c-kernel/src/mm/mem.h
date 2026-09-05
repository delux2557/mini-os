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
/* 映射一页到指定页目录（v0.11 每进程地址空间用）。
 * v0.30（BUG-033）：返回 0 成功 / -1 失败（页表帧 OOM 时未建立映射，不写物理 0）。
 * 调用方应检查返回值，失败时降级处理（如 pf_handler 栈生长转 STACK_BOOM）。 */
int  map_page_in(uint32_t pd, uint32_t virt, uint32_t phys, uint32_t flags);
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

/* ---- 用户地址空间布局（v0.26：槽从 8KB 扩到 32KB，栈可按需生长；v0.26#3 用户空间扩到 16MB） ----
 * 用户栈区 [USER_STACK_AREA_BASE, USER_STACK_AREA_END)：每进程 32KB 槽
 *   = [守卫页 4KB（不映射，槽底硬底，永不映射） | 可生长栈区 28KB（初始仅顶页映射）]
 * 栈从槽顶向下增长：ESP 下溢到"当前栈页下方 1 页"即命中守卫页 -> 页错误 ->
 * 内核判 STACK_GROWTH（补映射一页、守卫页随之下移）或 STACK_BOOM（深越界，栈溢出）。
 * 布局（v0.26 迁址 + v0.26#3 扩区）：栈区 [0x80010000, 0x80090000) -> shell 0x80090000 ->
 * app 区 [0x800A0000, 0x801A0000) 1MB（ELF 加载去上限）-> 共享内存 0x801A0000 ->
 * 堆区 [0x801A4000, 0x802A4000) 1MB（全部 < USER_SPACE_END 0x81000000）。
 * 共享内存区 [SHMEM_VBASE, +SLOTS*4K)：与 usermode.c 的 sys_shmem 严格一致，
 * 所有进程映射到同一物理帧，fork 时保持共享（不深拷贝，v0.12）。 */
#define USER_STACK_AREA_BASE  0x80010000u
#define USER_STACK_SLOT       0x8000u     /* 每进程栈区大小（守卫页 + 可生长栈区） */
#define USER_STACK_GUARD      0x1000u     /* 守卫页大小（槽内最低一页，永不映射） */
#define USER_STACK_PAGES      7           /* 可生长栈页数（SLOT/GUARD - 1，初始只映射 1 页） */
#define USER_STACK_MAX_PROCS  16          /* 用户进程槽数上限（与 MAX_PROCS 一致） */
#define USER_STACK_AREA_END   (USER_STACK_AREA_BASE + USER_STACK_MAX_PROCS * USER_STACK_SLOT)

/* ---- app 区（ELF 应用槽，v0.26#3 扩 MB 级）----
 * 普通应用链接到 APP_BASE（Makefile APP_ADDR / usermode.h APP_LINK 一致），
 * 加载时 mapfn 按 ELF 自身 PT_LOAD 范围逐页映射（不再受 16KB/8 帧上限约束）。
 * 1MB 空间足够单个应用（此前 16KB 槽+共享内存紧邻的结构债随之解除）。 */
#define USER_APP_BASE   0x800A0000u
#define USER_APP_SIZE   0x100000u        /* 1MB */
#define USER_APP_END    (USER_APP_BASE + USER_APP_SIZE)   /* 0x801A0000 */

#define SHMEM_VBASE   USER_APP_END       /* app 区之后 */
#define SHMEM_SLOTS   4

/* ---- 用户堆区（brk，v0.26#2）----
 * 堆区 [USER_HEAP_BASE, USER_HEAP_MAX)：在共享内存区之后向上生长。
 * 每进程独立（各自页目录映射各自物理帧，隔离），brk 记账进 PCB。
 * 扩展按页补映射（页对齐），收缩只更新 brk（保留映射复用）。
 * V3（minicc 自举）：80 页(320KB) 扩到 256 页(1MB)——自举版编译器（并行数组静态池 +
 * brk 动态 code/输入缓冲）编译自身需 ~800KB 堆；上限仍远低于 USER_SPACE_END。 */
#define USER_HEAP_PAGES 256           /* PCB.heap_frames 记账上限（1MB=256 页） */
#define USER_HEAP_BASE  0x801A4000u   /* 共享内存区之后 */
#define USER_HEAP_MAX   (USER_HEAP_BASE + USER_HEAP_PAGES * 0x1000u)  /* = 0x802A4000 */

/* 用户态页错误时的"栈事件"判定（guard.c 实现，纯逻辑可宿主单测）：
 *  - STACK_OK:     与"本进程栈"无关（fault 不在本进程槽内，或在已映射栈页内）
 *  - STACK_GROWTH: 命中"当前守卫页"（当前栈页下方 1 页，槽内还有空间）-> 可生长
 *  - STACK_BOOM:   深越界（越过当前守卫页再往下 / 已生长到上限）-> 栈溢出 */
typedef enum { STACK_OK = 0, STACK_GROWTH, STACK_BOOM } stack_evt_t;
stack_evt_t stack_guard_hit(uint32_t fault, uint32_t pid, uint32_t stack_bottom);

/* 物理帧分配器：返回物理地址（4KB 对齐） */
uint32_t frame_alloc(void);
uint32_t frame_alloc_run(uint32_t count); /* 分配 count 个连续帧 */
void frame_free(uint32_t phys);

/* v0.21 内核自审计：used_frames 与帧位图配平检查，返回失败项数（0=通过） */
uint32_t mem_audit(void);

/* 页错误处理（ISR 14 路由进来），懒分配区自动补页后可恢复 */
void pf_handler(registers_t *r);
uint32_t lazy_page_count(void);

#endif
