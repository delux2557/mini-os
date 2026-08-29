/* mini-os/v2-c-kernel/blockdev.c
 * 块设备抽象实现：当前后端为内存盘（一段连续内存，物理地址即线性地址，
 * 落在内核低 16MB 恒等映射区内）。 */
#include "blockdev.h"

void blockdev_init(blockdev_t *bd, uint8_t *base, uint32_t blocks) {
    bd->base   = base;
    bd->blocks = blocks;
}

void *blockdev_ptr(blockdev_t *bd, uint32_t blk, uint32_t off) {
    if (blk >= bd->blocks || off >= BLOCK_SIZE) return 0;
    return (void *)(bd->base + blk * BLOCK_SIZE + off);
}

void blockdev_read(blockdev_t *bd, uint32_t blk, uint32_t off, void *dst, uint32_t len) {
    const uint8_t *src = (const uint8_t *)blockdev_ptr(bd, blk, off);
    if (!src) return;
    uint8_t *d = (uint8_t *)dst;
    while (len--) *d++ = *src++;
}

void blockdev_write(blockdev_t *bd, uint32_t blk, uint32_t off, const void *src, uint32_t len) {
    uint8_t *dst = (uint8_t *)blockdev_ptr(bd, blk, off);
    if (!dst) return;
    const uint8_t *s = (const uint8_t *)src;
    while (len--) *dst++ = *s++;
}
