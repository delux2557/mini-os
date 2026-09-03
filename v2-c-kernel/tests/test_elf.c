/* mini-os/v2-c-kernel/tests/test_elf.c
 * ELF32 加载器宿主单元测试：只编译 src/elf.c（纯逻辑，mapfn 传 NULL）。
 * 验证：重定位/绝对加载、入口计算、PT_LOAD 段拷贝、bss 清零、mapfn 钩子、
 * 以及各类畸形输入的错误处理。
 *
 * 说明：宿主测试里 elf_load 会向任意 32 位地址写内存，因此把"目标区"做成
 * 一块静态 arena（.bss，-no-pie 下地址在 4GB 内），用 arena 地址作为
 * load_base 或段 vaddr，确保写入不越界、不破坏测试程序自身段。
 */
#include "utest.h"
#include "elf.h"
#include <string.h>

/* 与 src/elf.c 内一致的 ELF32 结构布局（构造测试镜像用） */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf32_hdr;

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

#define EHDR_SZ 52
#define PHDR_SZ 32
#define ET_EXEC 2
#define EM_386  3
#define PT_LOAD 1

/* 目标区：16 页，作为加载/写测的 arena */
static uint8_t arena[16 * 4096] __attribute__((aligned(4096)));

/* 构造标准两段 ELF：.text(text_vaddr, 4 字节) + .bss(bss_vaddr, 8 字节) */
static void build_elf(uint8_t *img, uint32_t text_vaddr, uint32_t bss_vaddr,
                      uint32_t entry) {
    memset(img, 0, 52 + 64 + 4);
    elf32_hdr *h = (elf32_hdr *)img;
    elf32_phdr *p0 = (elf32_phdr *)(img + EHDR_SZ);
    elf32_phdr *p1 = (elf32_phdr *)(img + EHDR_SZ + PHDR_SZ);

    memcpy(h->e_ident, "\x7F" "ELF", 4);
    h->e_ident[4] = 1;              /* ELFCLASS32 */
    h->e_type = ET_EXEC;
    h->e_machine = EM_386;
    h->e_version = 1;
    h->e_entry = entry;
    h->e_phoff = EHDR_SZ;
    h->e_ehsize = EHDR_SZ;
    h->e_phentsize = PHDR_SZ;
    h->e_phnum = 2;

    p0->p_type = PT_LOAD;
    p0->p_offset = EHDR_SZ + 2 * PHDR_SZ;   /* 紧跟程序头表 */
    p0->p_vaddr = text_vaddr;
    p0->p_filesz = 4;
    p0->p_memsz = 4;
    p0->p_flags = 5;                        /* R|X */
    p0->p_align = 0x1000;

    p1->p_type = PT_LOAD;
    p1->p_offset = 0;
    p1->p_vaddr = bss_vaddr;
    p1->p_filesz = 0;
    p1->p_memsz = 8;
    p1->p_flags = 6;                        /* R|W */
    p1->p_align = 0x1000;

    img[EHDR_SZ + 2 * PHDR_SZ + 0] = 0x90;  /* .text 内容 x4 */
    img[EHDR_SZ + 2 * PHDR_SZ + 1] = 0x90;
    img[EHDR_SZ + 2 * PHDR_SZ + 2] = 0x90;
    img[EHDR_SZ + 2 * PHDR_SZ + 3] = 0x90;
}

static uint32_t map_calls[8][2];
static int map_n;
static int my_map(uint32_t vaddr, uint32_t len) {
    if (map_n < 8) { map_calls[map_n][0] = vaddr; map_calls[map_n][1] = len; }
    map_n++;
    return 0;
}
/* 映射失败钩子：让 elf_load 在中途中止（不得再 memcpy/memset 未映射区） */
static int fail_map(uint32_t vaddr, uint32_t len) {
    (void)vaddr; (void)len;
    return -1;
}

int main(void) {
    uint8_t img[256];
    uint32_t entry = 0;
    uint32_t base = (uint32_t)arena;

    /* 1) 重定位加载（load_base=arena）：标准链接地址 0x08048000 搬到 arena。
     *    .text 拷入 arena[0]，.bss 清零到 arena[0x1000..0x1008]，入口=arena。 */
    memset(arena, 0xAA, sizeof(arena));
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    CHECK_EQ(elf_load(img, EHDR_SZ + 2 * PHDR_SZ + 4, base, NULL, &entry), 0);
    CHECK_EQ(entry, base);
    CHECK_EQ(*(uint32_t *)&arena[0], 0x90909090u);
    CHECK_EQ(*(uint32_t *)&arena[0x1000], 0x00000000u);
    CHECK_EQ(*(uint32_t *)&arena[0x1004], 0x00000000u);
    CHECK_EQ(arena[0x2000], 0xAA);           /* 越界区域未被触碰 */

    /* 2) mapfn 钩子：按段顺序收到 (dst, memsz)，memsz 用 p_memsz */
    map_n = 0;
    CHECK_EQ(elf_load(img, EHDR_SZ + 2 * PHDR_SZ + 4, base, my_map, &entry), 0);
    CHECK_EQ(map_n, 2);
    CHECK_EQ(map_calls[0][0], base);
    CHECK_EQ(map_calls[0][1], 4);
    CHECK_EQ(map_calls[1][0], base + 0x1000u);
    CHECK_EQ(map_calls[1][1], 8);

    /* 3) 绝对加载（load_base=0）：段按 vaddr 原样放置，入口即 e_entry。
     *    为安全，vaddr 直接用 arena 内地址。 */
    memset(arena, 0xAA, sizeof(arena));
    build_elf(img, base + 0x2000u, base + 0x3000u, base + 0x2000u);
    CHECK_EQ(elf_load(img, EHDR_SZ + 2 * PHDR_SZ + 4, 0, NULL, &entry), 0);
    CHECK_EQ(entry, base + 0x2000u);
    CHECK_EQ(*(uint32_t *)&arena[0x2000], 0x90909090u);
    CHECK_EQ(*(uint32_t *)&arena[0x3000], 0x00000000u);
    /* 段之间（0x2000 段尾 ~ 0x3000 段首）不被写入 */
    CHECK_EQ(*(uint32_t *)&arena[0x2400], 0xAAAAAAAAu);

    /* 4) 畸形输入错误处理 */
    /* 4.1 文件过短（不足文件头） */
    CHECK_EQ(elf_load(img, 4, base, NULL, &entry), -1);
    /* 4.2 魔数错误 */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    img[0] = 0x00;
    CHECK_EQ(elf_load(img, sizeof(img), base, NULL, &entry), -1);
    /* 4.3 64 位类 */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    img[4] = 2;
    CHECK_EQ(elf_load(img, sizeof(img), base, NULL, &entry), -1);
    /* 4.4 类型非 ET_EXEC（如 ET_DYN） */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    ((elf32_hdr *)img)->e_type = 3;
    CHECK_EQ(elf_load(img, sizeof(img), base, NULL, &entry), -1);
    /* 4.5 机器非 EM_386（如 EM_ARM=40） */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    ((elf32_hdr *)img)->e_machine = 40;
    CHECK_EQ(elf_load(img, sizeof(img), base, NULL, &entry), -1);
    /* 4.6 无程序头 */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    ((elf32_hdr *)img)->e_phnum = 0;
    CHECK_EQ(elf_load(img, sizeof(img), base, NULL, &entry), -1);
    /* 4.7 程序头表超出文件 */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    ((elf32_hdr *)img)->e_phoff = 0x7FF00000u;
    CHECK_EQ(elf_load(img, sizeof(img), base, NULL, &entry), -1);
    /* 4.8 无 PT_LOAD 段（两个 phdr 都改类型） */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    ((elf32_phdr *)(img + EHDR_SZ))->p_type = 0;
    ((elf32_phdr *)(img + EHDR_SZ + PHDR_SZ))->p_type = 0;
    CHECK_EQ(elf_load(img, sizeof(img), base, NULL, &entry), -1);
    /* 4.9 段数据越过文件末尾（p_offset+p_filesz > size） */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    CHECK_EQ(elf_load(img, EHDR_SZ + 2 * PHDR_SZ + 3, base, NULL, &entry), -1);
    /* 4.10 phentsize 过小 */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    ((elf32_hdr *)img)->e_phentsize = 20;
    CHECK_EQ(elf_load(img, sizeof(img), base, NULL, &entry), -1);
    /* 4.11 mapfn 映射失败：elf_load 必须中止（返回 -1），且不得再写目标区/清 bss */
    memset(arena, 0xAA, sizeof(arena));
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    CHECK_EQ(elf_load(img, sizeof(img), base, fail_map, &entry), -1);
    CHECK_EQ(arena[0], 0xAA);            /* .text 未写入 */
    CHECK_EQ(arena[0x1000], 0xAA);       /* bss 未清零 */

    /* 5) elf_load_range：返回页对齐的 PT_LOAD 覆盖区间（供 mapfn 决定映射范围）。
     *    纯解析，不写内存，可用任意链接地址。 */
    uint32_t lb = 0, le = 0;
    /* 5.1 标准两段：text@0x08048000 + bss@0x08049000(memsz=8)。
     *    bss 越入 0x08049000 页，区间端取到下一页 0x0804A000。 */
    build_elf(img, 0x08048000u, 0x08049000u, 0x08048000u);
    CHECK_EQ(elf_load_range(img, sizeof(img), &lb, &le), 0);
    CHECK_EQ(lb, 0x08048000u);
    CHECK_EQ(le, 0x0804A000u);
    /* 5.2 段未页对齐也向下取整（-Ttext 把 ELF 头放前页的场景，端同上取整） */
    build_elf(img, 0x0804A100u, 0x0804B000u, 0x0804A100u);
    CHECK_EQ(elf_load_range(img, sizeof(img), &lb, &le), 0);
    CHECK_EQ(lb, 0x0804A000u);
    CHECK_EQ(le, 0x0804C000u);
    /* 5.3 非法输入返回 -1 */
    CHECK_EQ(elf_load_range(img, 4, &lb, &le), -1);
    img[0] = 0;
    CHECK_EQ(elf_load_range(img, sizeof(img), &lb, &le), -1);
    build_elf(img, 0x0804A100u, 0x0804B000u, 0x0804A100u);
    ((elf32_phdr *)(img + EHDR_SZ))->p_type = 0;
    ((elf32_phdr *)(img + EHDR_SZ + PHDR_SZ))->p_type = 0;
    CHECK_EQ(elf_load_range(img, sizeof(img), &lb, &le), -1);

    UTEST_SUMMARY("test_elf");
}
