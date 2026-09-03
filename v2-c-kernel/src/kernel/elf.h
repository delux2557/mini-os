/* mini-os/v2-c-kernel/elf.h
 * ELF32 加载器（v0.9）：解析可执行文件、拷贝 PT_LOAD 段、返回入口。
 *  - 纯逻辑（只依赖内存拷贝），可在宿主环境单元测试（tests/test_elf.c）
 *  - 内核侧通过 mapfn 钩子为加载目标区映射物理页；
 *    宿主测试传 NULL（直接写入普通内存缓冲）。
 */
#ifndef _ELF_H
#define _ELF_H
#include <stdint.h>

/* 平台映射钩子：把 [vaddr, vaddr+len) 虚拟地址区映射到可用物理内存。
 * vaddr 可能未按页对齐，实现需自行向下取整到页并覆盖整段。宿主测试传 NULL。
 * 返回 0=成功 / 非 0=映射失败（elf_load 检测到失败会中止，不再拷贝/清零）。 */
typedef int (*elf_map_fn)(uint32_t vaddr, uint32_t len);

/* 加载 ELF32 可执行文件：
 *  - data/size：ELF 文件内容
 *  - load_base：加载基址。
 *      传 0   = 不重定位，段按 ELF 自带链接地址(vaddr)原样放置，入口即 e_entry；
 *      传非 0 = 重定位，把"镜像原点 link_base"搬到 load_base，
 *               段拷到 load_base+(vaddr-link_base)，入口=load_base+(e_entry-link_base)。
 *  - mapfn：见上；成功返回 0，入口写入 *entry_out；失败返回 -1。 */
int elf_load(const uint8_t *data, uint32_t size, uint32_t load_base,
             elf_map_fn mapfn, uint32_t *entry_out);

/* 仅扫描 PT_LOAD 段，返回页对齐后的 [*base, *end) 虚拟地址范围。
 * 内核侧用它决定 mapfn 应映射的区间（含 ELF 头所在页：链接器把 ELF 头放在
 * -Ttext 地址的前一页，如 -Ttext 0x80030000 时首个 PT_LOAD vaddr 为 0x8002f000）。
 * 无有效段或文件不合法返回 -1。 */
int elf_load_range(const uint8_t *data, uint32_t size,
                   uint32_t *base, uint32_t *end);

#endif
