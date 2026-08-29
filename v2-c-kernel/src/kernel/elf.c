/* mini-os/v2-c-kernel/elf.c
 * ELF32 加载器实现（v0.9）。
 *
 * 支持子集（教学内核所需）：
 *  - 32 位、ET_EXEC、EM_386
 *  - 只关心 PT_LOAD 段：按 p_vaddr 放置，filesz 拷入、memsz-filesz 清零（bss）
 *  - 不做重定位（用户程序以 -fno-pie 静态链接到固定地址）
 *
 * 为可测试性，把"写内存"与"映射内存"解耦：
 *  - 段拷贝目标 = load_base + (p_vaddr - link_base)，link_base 为最低 PT_LOAD vaddr（页对齐）
 *  - 内核侧 mapfn 负责把目标虚拟区映射成物理页；宿主测试传 NULL
 */
#include "elf.h"

#define EI_NIDENT 16

/* ELF32 文件头 */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf32_hdr;

/* ELF32 程序头 */
typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_phdr;

#define ELFCLASS32 1
#define ET_EXEC    2
#define EM_386     3
#define PT_LOAD    1
#define PAGE_MASK  0xFFFFF000u

static void memcpy_v(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}
static void memset_v(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

int elf_load(const uint8_t *data, uint32_t size, uint32_t load_base,
             elf_map_fn mapfn, uint32_t *entry_out) {
    if (size < sizeof(elf32_hdr)) return -1;

    const elf32_hdr *h = (const elf32_hdr *)data;
    if (h->e_ident[0] != 0x7F || h->e_ident[1] != 'E' ||
        h->e_ident[2] != 'L' || h->e_ident[3] != 'F') return -1;
    if (h->e_ident[4] != ELFCLASS32) return -1;
    if (h->e_type != ET_EXEC) return -1;
    if (h->e_machine != EM_386) return -1;
    if (h->e_phentsize < sizeof(elf32_phdr)) return -1;
    if (h->e_phnum == 0) return -1;
    /* 程序头表必须在文件范围内 */
    uint32_t ph_end = h->e_phoff + (uint32_t)h->e_phnum * h->e_phentsize;
    if (ph_end > size || ph_end < h->e_phoff) return -1;

    /* 计算 link_base：最低 PT_LOAD 的 vaddr（页对齐向下） */
    uint32_t link_base = 0xFFFFFFFFu;
    uint16_t i;
    for (i = 0; i < h->e_phnum; i++) {
        const elf32_phdr *ph = (const elf32_phdr *)(data + h->e_phoff + (uint32_t)i * h->e_phentsize);
        if (ph->p_type == PT_LOAD && ph->p_vaddr < link_base)
            link_base = ph->p_vaddr;
    }
    if (link_base == 0xFFFFFFFFu) return -1;   /* 无可加载段 */
    link_base &= PAGE_MASK;

    for (i = 0; i < h->e_phnum; i++) {
        const elf32_phdr *ph = (const elf32_phdr *)(data + h->e_phoff + (uint32_t)i * h->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        /* load_base==0：按 ELF 自带链接地址原样放置；否则把镜像原点 link_base 平移到 load_base */
        uint32_t dst = load_base ? load_base + (ph->p_vaddr - link_base) : ph->p_vaddr;
        uint32_t filesz = ph->p_filesz, memsz = ph->p_memsz;
        /* 段数据必须完整落在文件内 */
        if (ph->p_offset + filesz > size || ph->p_offset + filesz < ph->p_offset)
            return -1;
        if (mapfn) mapfn(dst, memsz);
        memcpy_v((void *)dst, data + ph->p_offset, filesz);
        if (memsz > filesz)
            memset_v((void *)(dst + filesz), 0, memsz - filesz);
    }

    if (entry_out)
        *entry_out = load_base ? load_base + (h->e_entry - link_base) : h->e_entry;
    return 0;
}

/* 扫描 PT_LOAD 段，返回页对齐后的 [*base, *end) 虚拟范围（供 mapfn 决定映射区间） */
int elf_load_range(const uint8_t *data, uint32_t size,
                   uint32_t *base, uint32_t *end) {
    if (size < sizeof(elf32_hdr)) return -1;

    const elf32_hdr *h = (const elf32_hdr *)data;
    if (h->e_ident[0] != 0x7F || h->e_ident[1] != 'E' ||
        h->e_ident[2] != 'L' || h->e_ident[3] != 'F') return -1;
    if (h->e_ident[4] != ELFCLASS32) return -1;
    if (h->e_phentsize < sizeof(elf32_phdr)) return -1;
    uint32_t ph_end = h->e_phoff + (uint32_t)h->e_phnum * h->e_phentsize;
    if (ph_end > size || ph_end < h->e_phoff) return -1;

    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    uint16_t i;
    for (i = 0; i < h->e_phnum; i++) {
        const elf32_phdr *ph = (const elf32_phdr *)(data + h->e_phoff + (uint32_t)i * h->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        uint32_t s = ph->p_vaddr & PAGE_MASK;
        uint32_t e = (ph->p_vaddr + ph->p_memsz + 0xFFFu) & PAGE_MASK;
        if (e < s) return -1;                       /* 溢出 */
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }
    if (lo == 0xFFFFFFFFu) return -1;               /* 无可加载段 */
    *base = lo;
    *end  = hi;
    return 0;
}
