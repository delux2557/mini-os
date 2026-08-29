/* mini-os/v2-c-kernel/src/ata.h
 * ATA PIO 驱动（v0.16）：主通道 master 盘，LBA28，轮询模式（不依赖中断）。
 * 扇区 512B；上层 FS 块（4096B）= 8 扇区，由调用方换算。
 */
#ifndef _ATA_H
#define _ATA_H
#include <stdint.h>

#define ATA_SECTOR_SIZE 512u

/* 探测主通道 master：有可用的 ATA 盘返回 1（并记录扇区数），否则返回 0 */
int ata_init(void);

/* 读/写 count 个扇区（自 lba 起，count<=255）；成功返回 0，失败返回 -1 */
int ata_read_sectors(uint32_t lba, uint32_t count, void *buf);
int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf);

/* 探测到的扇区总数（LBA28）；无盘时为 0 */
uint32_t ata_sectors(void);

#endif
