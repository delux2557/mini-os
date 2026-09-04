/* mini-os/v2-c-kernel/fs.c
 * 极简文件系统实现（v0.8，v0.14 增强）。
 * 磁盘布局（块 4KB）：
 *    块 0: 超级块（magic / 总块数 / inode 数）
 *    块 1: inode 位图（64 inode -> 8B，占 1 块）
 *    块 2: 数据块位图（<=252 块 -> 32B，占 1 块）
 *    块 3: inode 表（64 * 64B = 4KB，恰 1 块）
 *    块 4..: 数据块
 * v0.14 增强：
 *  - 目录层级：绝对路径 /a/b/c（支持 . 与 ..、重复/结尾斜杠），mkdir/rmdir（仅空目录）
 *  - 间接块：inode 增加 indirect 字段，单文件上限 12 直接块 + 1024 间接块
 * 纯逻辑：只经 blockdev 读写，不依赖内核/调度/串口，可在宿主环境单元测试。
 */
#include "fs.h"

#define SUPER_BLK        0u
#define INODE_BITMAP_BLK 1u
#define DATA_BITMAP_BLK  2u
#define INODE_TABLE_BLK  3u
#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(fs_inode_t))      /* 64 */
#define ENTRIES_PER_BLOCK (BLOCK_SIZE / sizeof(fs_dir_entry_t)) /* 128 */
#define PATH_DEPTH_MAX   16

static blockdev_t fs_dev;   /* 当前挂载的设备 */

void fs_mount(blockdev_t *bd) { fs_dev = *bd; }
blockdev_t *fs_device(void)   { return &fs_dev; }

static void memsetb(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}
static void memcpb(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}
static uint32_t str_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* ---- inode 存取 ---- */
static void inode_get(blockdev_t *bd, uint32_t i, fs_inode_t *out) {
    if (i >= FS_MAX_INODES) { memsetb(out, 0, sizeof(*out)); return; }
    memcpb(out, blockdev_ptr(bd, INODE_TABLE_BLK, i * sizeof(fs_inode_t)),
           sizeof(fs_inode_t));
}
static void inode_put(blockdev_t *bd, uint32_t i, const fs_inode_t *in) {
    if (i >= FS_MAX_INODES) return;
    memcpb(blockdev_ptr(bd, INODE_TABLE_BLK, i * sizeof(fs_inode_t)),
           in, sizeof(fs_inode_t));
}

/* ---- 位图 ---- */
static int bitmap_test(blockdev_t *bd, uint32_t blk, uint32_t bit) {
    uint8_t *b = (uint8_t *)blockdev_ptr(bd, blk, 0);
    return (b[bit >> 3] >> (bit & 7)) & 1;
}
static void bitmap_set(blockdev_t *bd, uint32_t blk, uint32_t bit, int v) {
    uint8_t *b = (uint8_t *)blockdev_ptr(bd, blk, 0);
    if (v) b[bit >> 3] |=  (uint8_t)(1u << (bit & 7));
    else   b[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}
static int bitmap_alloc(blockdev_t *bd, uint32_t blk, uint32_t nbits, uint32_t start) {
    for (uint32_t i = start; i < nbits; i++)
        if (!bitmap_test(bd, blk, i)) { bitmap_set(bd, blk, i, 1); return (int)i; }
    return -1;
}

static int alloc_inode(blockdev_t *bd) {
    return bitmap_alloc(bd, INODE_BITMAP_BLK, FS_MAX_INODES, 1);
}
static int alloc_block(blockdev_t *bd) {
    return bitmap_alloc(bd, DATA_BITMAP_BLK, bd->blocks, FS_DATA_START);
}

/* ---- 目录条目（作用于"指定目录 inode"，v0.14 起不再写死根目录） ---- */
/* 在目录 inode 中找空条目并写入；目录缺块时自动分配新块（即目录扩容） */
static int dir_add(blockdev_t *bd, uint32_t dir, const char *name, uint32_t inode) {
    fs_inode_t di;
    inode_get(bd, dir, &di);
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (di.blocks[b] == 0) {
            int blk = alloc_block(bd);
            if (blk < 0) return -1;
            memsetb(blockdev_ptr(bd, (uint32_t)blk, 0), 0, BLOCK_SIZE);
            di.blocks[b] = (uint32_t)blk;
            di.size += BLOCK_SIZE;
            inode_put(bd, dir, &di);
        }
        fs_dir_entry_t *e = (fs_dir_entry_t *)blockdev_ptr(bd, di.blocks[b], 0);
        for (uint32_t k = 0; k < ENTRIES_PER_BLOCK; k++) {
            if (e[k].inode == 0 && e[k].name[0] == 0) {
                memsetb(e[k].name, 0, FS_MAX_NAME);
                memcpb(e[k].name, name, str_len(name));
                e[k].type  = 0;
                e[k].inode = inode;
                return 0;
            }
        }
    }
    return -1;   /* 目录块用尽 */
}

static void dir_remove(blockdev_t *bd, uint32_t dir, const char *name) {
    fs_inode_t di;
    inode_get(bd, dir, &di);
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (!di.blocks[b]) continue;
        fs_dir_entry_t *e = (fs_dir_entry_t *)blockdev_ptr(bd, di.blocks[b], 0);
        for (uint32_t k = 0; k < ENTRIES_PER_BLOCK; k++) {
            if (e[k].inode && str_eq(e[k].name, name)) {
                memsetb(e[k].name, 0, FS_MAX_NAME);
                e[k].inode = 0;
                e[k].type  = 0;
                return;
            }
        }
    }
}

/* 目录是否为空（无任何有效条目） */
static int dir_empty(blockdev_t *bd, uint32_t dir) {
    fs_inode_t di;
    inode_get(bd, dir, &di);
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (!di.blocks[b]) continue;
        fs_dir_entry_t *e = (fs_dir_entry_t *)blockdev_ptr(bd, di.blocks[b], 0);
        for (uint32_t k = 0; k < ENTRIES_PER_BLOCK; k++)
            if (e[k].inode) return 0;
    }
    return 1;
}

/* ---- 路径解析 ----
 * 绝对路径 "/a/b/c"，支持：重复斜杠（"//"）、结尾斜杠、"."（当前目录）、".."（父目录）。
 * 解析过程用显式目录栈记录经过的目录，使 ".." 可回退（根目录的 ".." 仍是根）。
 * 输出：*dirout = 叶子所在目录 inode；*leaf = 叶子名（路径以 '/' 结尾时为空串）。
 * 返回：叶子 inode（存在）或 -1（叶子不存在 / 中间组件不存在或非目录 / 层级过深）。
 */
static int fs_walk(blockdev_t *bd, const char *path, uint32_t *dirout,
                   char *leaf, uint32_t leafmax) {
    uint32_t dir = FS_ROOT_INODE;
    uint32_t stack[PATH_DEPTH_MAX];
    int sp = 0;
    if (sp < PATH_DEPTH_MAX) stack[sp++] = dir;
    const char *p = path;
    char comp[FS_MAX_NAME];

    for (;;) {
        while (*p == '/') p++;
        if (*p == 0) {                 /* 路径结束：无叶子组件 */
            leaf[0] = 0;
            *dirout = dir;
            return (int)dir;
        }
        uint32_t cl = 0;
        while (*p && *p != '/' && cl < FS_MAX_NAME - 1) comp[cl++] = *p++;
        comp[cl] = 0;
        if (cl == 0) continue;         /* 不可能（上面已跳过 '/'） */
        if (str_eq(comp, ".")) continue;
        if (str_eq(comp, "..")) {
            if (sp > 1) { sp--; dir = stack[sp - 1]; }
            continue;
        }
        int ino = fs_lookup_in(bd, dir, comp);
        if (ino < 0) {
            /* 组件不存在：仅当它是最后一个组件时才作为"叶子"返回；
             * 否则为非法路径（必须把 leaf 置空 + 输出 dir，供调用方区分）。 */
            const char *q = p;
            while (*q == '/') q++;
            if (*q != 0) { leaf[0] = 0; *dirout = dir; return -1; }
            if (cl >= leafmax) return -1;
            memcpb(leaf, comp, cl); leaf[cl] = 0;
            *dirout = dir;
            return -1;
        }
        /* 组件存在：若是最后一个组件即叶子；否则必须是目录 */
        const char *q = p;
        while (*q == '/') q++;
        if (*q == 0) {                 /* 该组件是叶子 */
            if (cl >= leafmax) return -1;
            memcpb(leaf, comp, cl); leaf[cl] = 0;
            *dirout = dir;
            return ino;
        }
        fs_inode_t in;
        inode_get(bd, (uint32_t)ino, &in);
        if (in.type != FS_TYPE_DIR) { leaf[0] = 0; *dirout = dir; return -1; } /* 中间组件不是目录 */
        dir = (uint32_t)ino;
        if (sp < PATH_DEPTH_MAX) stack[sp++] = dir;
        else { leaf[0] = 0; *dirout = dir; return -1; }   /* 层级过深 */
    }
}

/* ---- 格式化 ---- */
int fs_init(blockdev_t *bd) {
    if (bd->blocks <= FS_DATA_START) return -1;
    /* 超级块 */
    uint32_t *super = (uint32_t *)blockdev_ptr(bd, SUPER_BLK, 0);
    super[0] = FS_MAGIC;
    super[1] = bd->blocks;
    super[2] = FS_MAX_INODES;
    /* 清位图与 inode 表 */
    memsetb(blockdev_ptr(bd, INODE_BITMAP_BLK, 0), 0, BLOCK_SIZE);
    memsetb(blockdev_ptr(bd, DATA_BITMAP_BLK, 0), 0, BLOCK_SIZE);
    memsetb(blockdev_ptr(bd, INODE_TABLE_BLK, 0), 0, BLOCK_SIZE);
    /* inode 0 = 根目录 */
    bitmap_set(bd, INODE_BITMAP_BLK, FS_ROOT_INODE, 1);
    fs_inode_t root;
    memsetb(&root, 0, sizeof(root));
    root.type  = FS_TYPE_DIR;
    root.links = 1;
    inode_put(bd, FS_ROOT_INODE, &root);
    return 0;
}

int fs_lookup_in(blockdev_t *bd, uint32_t dir, const char *name) {
    fs_inode_t di;
    inode_get(bd, dir, &di);
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (!di.blocks[b]) continue;
        fs_dir_entry_t *e = (fs_dir_entry_t *)blockdev_ptr(bd, di.blocks[b], 0);
        for (uint32_t k = 0; k < ENTRIES_PER_BLOCK; k++)
            if (e[k].inode && str_eq(e[k].name, name))
                return (int)e[k].inode;
    }
    return -1;
}

int fs_lookup(blockdev_t *bd, const char *path) {
    uint32_t dir;
    char leaf[FS_MAX_NAME];
    /* fs_walk 对"路径即目录（无叶子）"返回目录 inode（>=0）；
     * 叶子缺失/非法路径返回 -1。故直接返回其结果。 */
    (void)dir; (void)leaf;
    return fs_walk(bd, path, &dir, leaf, sizeof(leaf));
}

/* 在父目录下建对象（文件/目录共用）：返回新 inode 或 -1 */
static int fs_make(blockdev_t *bd, const char *path, uint16_t type) {
    uint32_t dir;
    char leaf[FS_MAX_NAME];
    int ino = fs_walk(bd, path, &dir, leaf, sizeof(leaf));
    if (leaf[0] == 0) return -1;        /* 不能建根目录或以 '/' 结尾 */
    if (ino >= 0) return -1;            /* 已存在 */
    uint32_t nl = str_len(leaf);
    if (nl == 0 || nl >= FS_MAX_NAME) return -1;
    int ni = alloc_inode(bd);
    if (ni < 0) return -1;
    fs_inode_t in;
    memsetb(&in, 0, sizeof(in));
    in.type  = type;
    in.links = 1;
    inode_put(bd, (uint32_t)ni, &in);
    if (dir_add(bd, dir, leaf, (uint32_t)ni) < 0) {
        bitmap_set(bd, INODE_BITMAP_BLK, (uint32_t)ni, 0);   /* 回滚 */
        return -1;
    }
    return ni;
}

int fs_create(blockdev_t *bd, const char *path) {
    return fs_make(bd, path, FS_TYPE_FILE);
}

int fs_mkdir(blockdev_t *bd, const char *path) {
    return fs_make(bd, path, FS_TYPE_DIR);
}

/* ---- BUG-057 两级权限：系统文件只读 / 用户文件可写 ----
 * 权威判定集中在 fs 层（不依赖 syscall 调用者），fs_delete/fs_rmdir/fs_write 均据此拦截，
 * 用户无从绕过。仅需位运算，不引入 strcpy/sprintf。 */
int fs_protect(blockdev_t *bd, const char *path) {
    int ino = fs_lookup(bd, path);
    if (ino < 0) return -1;
    fs_inode_t in;
    inode_get(bd, (uint32_t)ino, &in);
    in.mode |= FS_MODE_RO;                 /* 幂等：多次 protect 无副作用 */
    inode_put(bd, (uint32_t)ino, &in);
    return 0;
}

int fs_is_ro(blockdev_t *bd, uint32_t inode) {
    if (inode >= FS_MAX_INODES) return -1; /* 非法 inode */
    fs_inode_t in;
    inode_get(bd, inode, &in);
    return (in.mode & FS_MODE_RO) ? 1 : 0;
}

/* 释放文件/目录占用的数据块（直接 + 间接 + 间接块本身） */
static void free_inode_blocks(blockdev_t *bd, fs_inode_t *in) {
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (in->blocks[b]) {
            bitmap_set(bd, DATA_BITMAP_BLK, in->blocks[b], 0);
            in->blocks[b] = 0;
        }
    }
    if (in->indirect) {
        uint32_t *ptrs = (uint32_t *)blockdev_ptr(bd, in->indirect, 0);
        for (uint32_t k = 0; k < FS_INDIRECT_BLOCKS; k++)
            if (ptrs[k]) bitmap_set(bd, DATA_BITMAP_BLK, ptrs[k], 0);
        bitmap_set(bd, DATA_BITMAP_BLK, in->indirect, 0);
        in->indirect = 0;
    }
}

int fs_delete(blockdev_t *bd, const char *path) {
    uint32_t dir;
    char leaf[FS_MAX_NAME];
    int ino = fs_walk(bd, path, &dir, leaf, sizeof(leaf));
    if (ino < 0 || leaf[0] == 0) return -1;
    fs_inode_t in;
    inode_get(bd, (uint32_t)ino, &in);
    if (in.type != FS_TYPE_FILE) return -1;   /* 目录用 fs_rmdir */
    if (in.mode & FS_MODE_RO) return -1;      /* BUG-057：只读系统文件不可删 */
    free_inode_blocks(bd, &in);
    memsetb(&in, 0, sizeof(in));
    inode_put(bd, (uint32_t)ino, &in);
    dir_remove(bd, dir, leaf);
    bitmap_set(bd, INODE_BITMAP_BLK, (uint32_t)ino, 0);
    return 0;
}

int fs_rmdir(blockdev_t *bd, const char *path) {
    uint32_t dir;
    char leaf[FS_MAX_NAME];
    int ino = fs_walk(bd, path, &dir, leaf, sizeof(leaf));
    if (ino < 0 || leaf[0] == 0) return -1;
    fs_inode_t in;
    inode_get(bd, (uint32_t)ino, &in);
    if (in.type != FS_TYPE_DIR) return -1;    /* 非目录 */
    if (in.mode & FS_MODE_RO) return -1;      /* BUG-057：只读目录不可删 */
    if (!dir_empty(bd, (uint32_t)ino)) return -1;  /* 非空目录 */
    free_inode_blocks(bd, &in);
    memsetb(&in, 0, sizeof(in));
    inode_put(bd, (uint32_t)ino, &in);
    dir_remove(bd, dir, leaf);
    bitmap_set(bd, INODE_BITMAP_BLK, (uint32_t)ino, 0);
    return 0;
}

uint32_t fs_size(blockdev_t *bd, uint32_t inode) {
    fs_inode_t in;
    inode_get(bd, inode, &in);
    return in.size;
}

/* 返回文件第 b 个数据块号（不存在返回 0）；create=1 时惰性分配（含间接块） */
static uint32_t file_block(blockdev_t *bd, fs_inode_t *in, uint32_t b, int create) {
    if (b < FS_DIRECT_BLOCKS) {
        if (in->blocks[b] == 0) {
            if (!create) return 0;
            int blk = alloc_block(bd);
            if (blk < 0) return 0;
            memsetb(blockdev_ptr(bd, (uint32_t)blk, 0), 0, BLOCK_SIZE);
            in->blocks[b] = (uint32_t)blk;
        }
        return in->blocks[b];
    }
    uint32_t ib = b - FS_DIRECT_BLOCKS;
    if (ib >= FS_INDIRECT_BLOCKS) return 0;
    if (in->indirect == 0) {
        if (!create) return 0;
        int blk = alloc_block(bd);
        if (blk < 0) return 0;
        memsetb(blockdev_ptr(bd, (uint32_t)blk, 0), 0, BLOCK_SIZE);
        in->indirect = (uint32_t)blk;
    }
    uint32_t *ptrs = (uint32_t *)blockdev_ptr(bd, in->indirect, 0);
    if (ptrs[ib] == 0) {
        if (!create) return 0;
        int blk = alloc_block(bd);
        if (blk < 0) return 0;
        memsetb(blockdev_ptr(bd, (uint32_t)blk, 0), 0, BLOCK_SIZE);
        ptrs[ib] = (uint32_t)blk;
    }
    return ptrs[ib];
}

int fs_read(blockdev_t *bd, uint32_t inode, void *buf, uint32_t off, uint32_t len) {
    fs_inode_t in;
    inode_get(bd, inode, &in);
    if (in.type != FS_TYPE_FILE) return -1;
    if (off >= in.size) return 0;
    if (len > in.size - off) len = in.size - off;
    uint32_t done = 0;
    uint8_t *out = (uint8_t *)buf;
    while (done < len) {
        uint32_t bytepos = off + done;
        uint32_t b = bytepos / BLOCK_SIZE;
        uint32_t blk = file_block(bd, &in, b, 0);
        if (blk == 0) break;
        uint32_t boff = bytepos % BLOCK_SIZE;
        uint32_t n = len - done;
        if (n > BLOCK_SIZE - boff) n = BLOCK_SIZE - boff;
        blockdev_read(bd, blk, boff, out + done, n);
        done += n;
    }
    return (int)done;
}

int fs_write(blockdev_t *bd, uint32_t inode, const void *buf, uint32_t off, uint32_t len) {
    if (len == 0) return 0;
    fs_inode_t in;
    inode_get(bd, inode, &in);
    if (in.type != FS_TYPE_FILE) return -1;
    if (in.mode & FS_MODE_RO) return -1;      /* BUG-057：只读系统文件不可写 */
    if (off >= FS_MAX_FILE_SIZE) return -1;
    if (len > FS_MAX_FILE_SIZE - off) len = FS_MAX_FILE_SIZE - off;

    /* 确保覆盖范围内所有块已分配（file_block 对新块清零） */
    uint32_t first_b = off / BLOCK_SIZE;
    uint32_t last_b  = (off + len - 1) / BLOCK_SIZE;
    for (uint32_t b = first_b; b <= last_b; b++) {
        if (file_block(bd, &in, b, 1) == 0) {
            if (b == first_b) { inode_put(bd, inode, &in); return -1; }
            len = b * BLOCK_SIZE - off;   /* 只写能写进去的部分 */
            /* 短写语义：中间块分配失败时，已写入的部分不可回滚——已持久化到文件，
             * 且下方 in.size 会更新为 off+done。调用方必须检查返回值 != len 并自行降级 */
            break;
        }
    }

    uint32_t done = 0;
    const uint8_t *src = (const uint8_t *)buf;
    while (done < len) {
        uint32_t bytepos = off + done;
        uint32_t b = bytepos / BLOCK_SIZE;
        uint32_t boff = bytepos % BLOCK_SIZE;
        uint32_t n = len - done;
        if (n > BLOCK_SIZE - boff) n = BLOCK_SIZE - boff;
        blockdev_write(bd, file_block(bd, &in, b, 0), boff, src + done, n);
        done += n;
    }
    if (off + done > in.size) in.size = off + done;
    inode_put(bd, inode, &in);
    return (int)done;
}

int fs_list_dir(blockdev_t *bd, uint32_t dir, fs_dir_entry_t *out, uint32_t max) {
    fs_inode_t di;
    inode_get(bd, dir, &di);
    uint32_t n = 0;
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (!di.blocks[b]) continue;
        fs_dir_entry_t *e = (fs_dir_entry_t *)blockdev_ptr(bd, di.blocks[b], 0);
        for (uint32_t k = 0; k < ENTRIES_PER_BLOCK && n < max; k++) {
            if (e[k].inode) {
                memsetb(out[n].name, 0, FS_MAX_NAME);
                memcpb(out[n].name, e[k].name, str_len(e[k].name));
                out[n].inode = e[k].inode;
                fs_inode_t in;
                inode_get(bd, e[k].inode, &in);
                out[n].type = in.type;
                n++;
            }
        }
    }
    return (int)n;
}

int fs_list(blockdev_t *bd, const char *path, fs_dir_entry_t *out, uint32_t max) {
    uint32_t dir;
    char leaf[FS_MAX_NAME];
    int ino = fs_walk(bd, path, &dir, leaf, sizeof(leaf));
    if (ino < 0) return -1;    /* 叶子缺失或非法路径 */
    fs_inode_t in;
    inode_get(bd, (uint32_t)ino, &in);
    if (in.type != FS_TYPE_DIR) return -1;       /* 叶子是文件，不是目录 */
    return fs_list_dir(bd, (uint32_t)ino, out, max);
}
