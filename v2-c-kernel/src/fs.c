/* mini-os/v2-c-kernel/fs.c
 * 极简文件系统实现（v0.8）。
 * 磁盘布局（块 4KB）：
 *    块 0: 超级块（magic / 总块数 / inode 数）
 *    块 1: inode 位图（64 inode -> 8B，占 1 块）
 *    块 2: 数据块位图（<=252 块 -> 32B，占 1 块）
 *    块 3: inode 表（64 * 64B = 4KB，恰 1 块）
 *    块 4..: 数据块
 * 纯逻辑：只经 blockdev 读写，不依赖内核/调度/串口，可在宿主环境单元测试。
 */
#include "fs.h"

#define SUPER_BLK        0u
#define INODE_BITMAP_BLK 1u
#define DATA_BITMAP_BLK  2u
#define INODE_TABLE_BLK  3u
#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(fs_inode_t))      /* 64 */
#define ENTRIES_PER_BLOCK (BLOCK_SIZE / sizeof(fs_dir_entry_t)) /* 128 */
#define MAX_FILE_SIZE    (FS_DIRECT_BLOCKS * BLOCK_SIZE)

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

/* ---- 根目录条目 ---- */
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
                return;
            }
        }
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

int fs_lookup(blockdev_t *bd, const char *name) {
    fs_inode_t di;
    inode_get(bd, FS_ROOT_INODE, &di);
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (!di.blocks[b]) continue;
        fs_dir_entry_t *e = (fs_dir_entry_t *)blockdev_ptr(bd, di.blocks[b], 0);
        for (uint32_t k = 0; k < ENTRIES_PER_BLOCK; k++)
            if (e[k].inode && str_eq(e[k].name, name))
                return (int)e[k].inode;
    }
    return -1;
}

int fs_create(blockdev_t *bd, const char *name) {
    uint32_t nl = str_len(name);
    if (nl == 0 || nl >= FS_MAX_NAME) return -1;
    if (fs_lookup(bd, name) >= 0) return -1;   /* 重名 */
    int ino = alloc_inode(bd);
    if (ino < 0) return -1;
    fs_inode_t in;
    memsetb(&in, 0, sizeof(in));
    in.type  = FS_TYPE_FILE;
    in.links = 1;
    inode_put(bd, (uint32_t)ino, &in);
    if (dir_add(bd, FS_ROOT_INODE, name, (uint32_t)ino) < 0) {
        bitmap_set(bd, INODE_BITMAP_BLK, (uint32_t)ino, 0);   /* 回滚 */
        return -1;
    }
    return ino;
}

int fs_delete(blockdev_t *bd, const char *name) {
    int ino = fs_lookup(bd, name);
    if (ino < 0) return -1;
    fs_inode_t in;
    inode_get(bd, (uint32_t)ino, &in);
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (in.blocks[b]) {
            bitmap_set(bd, DATA_BITMAP_BLK, in.blocks[b], 0);
            in.blocks[b] = 0;
        }
    }
    inode_put(bd, (uint32_t)ino, &in);
    dir_remove(bd, FS_ROOT_INODE, name);
    bitmap_set(bd, INODE_BITMAP_BLK, (uint32_t)ino, 0);
    return 0;
}

uint32_t fs_size(blockdev_t *bd, uint32_t inode) {
    fs_inode_t in;
    inode_get(bd, inode, &in);
    return in.size;
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
        if (b >= FS_DIRECT_BLOCKS || in.blocks[b] == 0) break;
        uint32_t boff = bytepos % BLOCK_SIZE;
        uint32_t n = len - done;
        if (n > BLOCK_SIZE - boff) n = BLOCK_SIZE - boff;
        blockdev_read(bd, in.blocks[b], boff, out + done, n);
        done += n;
    }
    return (int)done;
}

int fs_write(blockdev_t *bd, uint32_t inode, const void *buf, uint32_t off, uint32_t len) {
    if (len == 0) return 0;
    fs_inode_t in;
    inode_get(bd, inode, &in);
    if (in.type != FS_TYPE_FILE) return -1;
    if (off >= MAX_FILE_SIZE) return -1;
    if (len > MAX_FILE_SIZE - off) len = MAX_FILE_SIZE - off;

    /* 确保覆盖范围内所有块已分配（新块清零） */
    uint32_t first_b = off / BLOCK_SIZE;
    uint32_t last_b  = (off + len - 1) / BLOCK_SIZE;
    for (uint32_t b = first_b; b <= last_b; b++) {
        if (in.blocks[b] == 0) {
            int blk = alloc_block(bd);
            if (blk < 0) {
                if (b == first_b) return -1;
                len = b * BLOCK_SIZE - off;   /* 只写能写进去的部分 */
                break;
            }
            memsetb(blockdev_ptr(bd, (uint32_t)blk, 0), 0, BLOCK_SIZE);
            in.blocks[b] = (uint32_t)blk;
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
        blockdev_write(bd, in.blocks[b], boff, src + done, n);
        done += n;
    }
    if (off + done > in.size) in.size = off + done;
    inode_put(bd, inode, &in);
    return (int)done;
}

int fs_list(blockdev_t *bd, fs_dir_entry_t *out, uint32_t max) {
    fs_inode_t di;
    inode_get(bd, FS_ROOT_INODE, &di);
    uint32_t n = 0;
    for (uint32_t b = 0; b < FS_DIRECT_BLOCKS; b++) {
        if (!di.blocks[b]) continue;
        fs_dir_entry_t *e = (fs_dir_entry_t *)blockdev_ptr(bd, di.blocks[b], 0);
        for (uint32_t k = 0; k < ENTRIES_PER_BLOCK && n < max; k++) {
            if (e[k].inode) {
                memsetb(out[n].name, 0, FS_MAX_NAME);
                memcpb(out[n].name, e[k].name, str_len(e[k].name));
                out[n].inode = e[k].inode;
                n++;
            }
        }
    }
    return (int)n;
}
