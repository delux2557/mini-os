/* mini-os/v2-c-kernel/fs.h
 * 极简文件系统（v0.8）：类 Unix 磁盘布局
 *   超级块 / inode 位图 / 数据块位图 / inode 表 / 数据区
 *  - 只支持根目录，文件名 <= 23 字符
 *  - 直接块映射：单文件最大 FS_DIRECT_BLOCKS * 块大小
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

typedef struct {
    uint32_t size;
    uint16_t type;
    uint16_t links;
    uint32_t blocks[FS_DIRECT_BLOCKS];  /* 数据块号，0=未分配 */
} fs_inode_t;

typedef struct {
    char     name[FS_MAX_NAME];
    uint32_t inode;
} fs_dir_entry_t;

/* 挂载：把某块设备交给文件系统管理（内核初始化时调用） */
void fs_mount(blockdev_t *bd);
blockdev_t *fs_device(void);

/* 格式化内存盘：写超级块、清位图、建根目录 */
int  fs_init(blockdev_t *bd);
int  fs_create(blockdev_t *bd, const char *name);      /* 根目录建文件，返回 inode 或 -1 */
int  fs_lookup(blockdev_t *bd, const char *name);      /* 返回 inode 或 -1 */
int  fs_delete(blockdev_t *bd, const char *name);      /* 0 或 -1 */
uint32_t fs_size(blockdev_t *bd, uint32_t inode);
int  fs_read (blockdev_t *bd, uint32_t inode, void *buf, uint32_t off, uint32_t len);
int  fs_write(blockdev_t *bd, uint32_t inode, const void *buf, uint32_t off, uint32_t len);
int  fs_list (blockdev_t *bd, fs_dir_entry_t *out, uint32_t max);

#endif
