/* mini-os/v2-c-kernel/fs.h
 * 极简文件系统（v0.8，v0.14 增强）：类 Unix 磁盘布局
 *   超级块 / inode 位图 / 数据块位图 / inode 表 / 数据区
 *  - v0.14 起支持目录层级（绝对路径 /a/b/c、. 与 ..、重复斜杠）
 *  - v0.14 起支持间接块：单文件 12 直接块 + 1 间接块（1024 个块号）
 *  - 纯逻辑（只依赖 blockdev），可在宿主环境单元测试（tests/test_fs.c）
 */
#ifndef _FS_H
#define _FS_H
#include <stdint.h>
#include "blockdev.h"

#define FS_MAGIC        0x4D494E49u   /* "MINI" */
#define FS_MAX_INODES   64
#define FS_MAX_NAME     24            /* 含结尾 '\0' */
#define FS_DIRECT_BLOCKS 12
#define FS_TYPE_FILE    0x01
#define FS_TYPE_DIR     0x02

#define FS_ROOT_INODE   0
#define FS_DATA_START   4u            /* 数据块起始块号 */

/* v0.14 间接块：一块存 4096/4=1024 个块号；单文件上限 12+1024 块 ≈ 4.1MB */
#define FS_INDIRECT_BLOCKS (BLOCK_SIZE / 4u)
#define FS_MAX_FILE_SIZE  ((FS_DIRECT_BLOCKS + FS_INDIRECT_BLOCKS) * BLOCK_SIZE)

typedef struct {
    uint32_t size;
    uint16_t type;
    uint16_t links;
    uint32_t blocks[FS_DIRECT_BLOCKS];  /* 直接数据块号，0=未分配 */
    uint32_t indirect;                  /* v0.14 间接块号（指向存块号的块），0=无 */
    uint16_t pad;                       /* 补齐（老镜像 pad 恒 0，见 BUG-057 兼容注记） */
    uint16_t mode;                      /* v0.34 BUG-057 权限位；0=可写，见 FS_MODE_RO */
} fs_inode_t;

/* BUG-057 两级权限：系统文件只读 / 用户文件可写。
 * mode 复用原 pad 的低 16 位（磁盘布局字节数不变，稳定 64B）；
 * 老镜像 pad 由创建时 memsetb 清零保证为 0 --> mode==0（可写），升级无缝。 */
#define FS_MODE_RO  0x0001u   /* 受保护/只读：不可 delete/rmdir/write */

typedef struct {
    char     name[FS_MAX_NAME];
    uint16_t type;      /* v0.14：条目类型（FS_TYPE_FILE/DIR） */
    uint16_t pad;
    uint32_t inode;
} fs_dir_entry_t;

/* 挂载：把某块设备交给文件系统管理（内核初始化时调用） */
void fs_mount(blockdev_t *bd);
blockdev_t *fs_device(void);

/* 格式化内存盘：写超级块、清位图、建根目录 */
int  fs_init(blockdev_t *bd);

/* 文件操作（name 或绝对路径，v0.14 起支持路径） */
int  fs_create(blockdev_t *bd, const char *path);      /* 建文件，返回 inode 或 -1 */
int  fs_lookup(blockdev_t *bd, const char *path);      /* 返回 inode 或 -1 */
int  fs_delete(blockdev_t *bd, const char *path);      /* 删文件（目录用 fs_rmdir） */
uint32_t fs_size(blockdev_t *bd, uint32_t inode);
int  fs_read (blockdev_t *bd, uint32_t inode, void *buf, uint32_t off, uint32_t len);
int  fs_write(blockdev_t *bd, uint32_t inode, const void *buf, uint32_t off, uint32_t len);
int  fs_list (blockdev_t *bd, const char *path, fs_dir_entry_t *out, uint32_t max);

/* v0.14 目录层级 */
int  fs_mkdir(blockdev_t *bd, const char *path);    /* 建目录（父目录须存在，返回 inode 或 -1） */
int  fs_rmdir(blockdev_t *bd, const char *path);    /* 删空目录（非空/非目录返回 -1） */
int  fs_lookup_in(blockdev_t *bd, uint32_t dir, const char *name);  /* 在指定目录查条目 */
int  fs_list_dir(blockdev_t *bd, uint32_t dir, fs_dir_entry_t *out, uint32_t max);

/* BUG-057 两级权限：系统文件只读 / 用户文件可写 */
int  fs_protect(blockdev_t *bd, const char *path);  /* 置目标为只读（返回 0/-1） */
int  fs_is_ro(blockdev_t *bd, uint32_t inode);      /* 只读=1，可写=0，inode 非法=-1 */

/* RD5（BUG-071）块归属账本：块 → owner inode，挡"合法范围内重复块"跨文件读写/RO 绕过 */
void     fs_scan_owners(blockdev_t *bd);        /* 挂载外部镜像后重建归属账本（扫描在用 inode 全部块） */
uint32_t fs_owner_violations_get(void);         /* 归属冲突/违约累计次数（仅观测，不入 audit bad 和） */
void     fs_owner_reset(void);                  /* 清计数（宿主测试用） */
uint32_t fs_owner_orphans(blockdev_t *bd);      /* 孤儿块数：位图在用但账本无主（0=健康） */

#endif
