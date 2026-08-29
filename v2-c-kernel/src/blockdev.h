/* mini-os/v2-c-kernel/blockdev.h
 * 块设备抽象（v0.8）：
 *  - 以"块"为单位的读写接口，屏蔽后端差异
 *  - 当前后端：内存盘（ramdisk），数据区是一段连续内存，可直接寻址
 *  - 纯逻辑（不依赖内核/硬件），可在宿主环境编译单元测试（tests/test_fs.c）
 */
#ifndef _BLOCKDEV_H
#define _BLOCKDEV_H
#include <stdint.h>

#define BLOCK_SIZE 4096u

typedef struct {
    uint8_t  *base;     /* 数据区基址（线性地址） */
    uint32_t  blocks;   /* 块总数 */
} blockdev_t;

void blockdev_init(blockdev_t *bd, uint8_t *base, uint32_t blocks);

/* 以块为单位读写；off/len 限制在本块内（off+len <= BLOCK_SIZE） */
void blockdev_read (blockdev_t *bd, uint32_t blk, uint32_t off, void *dst, uint32_t len);
void blockdev_write(blockdev_t *bd, uint32_t blk, uint32_t off, const void *src, uint32_t len);

/* 返回块内偏移的直接指针（避免来回拷贝）；越界返回 0 */
void *blockdev_ptr(blockdev_t *bd, uint32_t blk, uint32_t off);

#endif
