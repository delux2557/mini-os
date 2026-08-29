/* mini-os/v2-c-kernel/src/ata.c
 * ATA PIO 驱动（v0.16）：主通道 master 盘，LBA28，轮询模式。
 *   - 端口：数据 0x1F0 / 特性 0x1F1 / 扇区数 0x1F2 / LBA 0x1F3-0x1F5 /
 *           盘选 0x1F6 / 状态|命令 0x1F7；控制 0x3F6
 *   - 读：0x20，写：0x30，IDENTIFY：0xEC
 *   - 仅实现 PIO（无 DMA、无中断），QEMU 与多数真实 IDE 控制器均支持
 */
#include "ata.h"
#include "serial.h"

#define ATA_IO_BASE   0x1F0u
#define ATA_CTRL_BASE 0x3F6u
#define ATA_DATA     (ATA_IO_BASE + 0u)
#define ATA_ERR      (ATA_IO_BASE + 1u)
#define ATA_SECTORS  (ATA_IO_BASE + 2u)
#define ATA_LBA_LO   (ATA_IO_BASE + 3u)
#define ATA_LBA_MID  (ATA_IO_BASE + 4u)
#define ATA_LBA_HI   (ATA_IO_BASE + 5u)
#define ATA_DRIVE    (ATA_IO_BASE + 6u)
#define ATA_STATUS   (ATA_IO_BASE + 7u)
#define ATA_CMD      (ATA_IO_BASE + 7u)

#define ATA_SR_BSY  0x80u
#define ATA_SR_DRQ  0x08u
#define ATA_SR_ERR  0x01u

#define ATA_CMD_READ     0x20u
#define ATA_CMD_WRITE    0x30u
#define ATA_CMD_IDENTIFY 0xECu

static uint32_t disk_sectors = 0;
static int disk_present = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* 等待 BSY 清除（附带 ERR 提前失败与超时保护）；0=就绪，-1=出错/超时 */
static int ata_wait_ready(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & ATA_SR_BSY)) {
            if (st & ATA_SR_ERR) return -1;
            return 0;
        }
    }
    return -1;
}

/* 选盘 + 写 LBA28 参数（扇区数/lba/命令），返回 0=已发出命令 */
static int ata_select(uint32_t lba, uint32_t count, uint8_t cmd) {
    outb(ATA_DRIVE, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));   /* master + LBA28 高 4 位 */
    outb(ATA_SECTORS, (uint8_t)count);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_CMD, cmd);
    return 0;
}

int ata_read_sectors(uint32_t lba, uint32_t count, void *buf) {
    if (!disk_present || count == 0 || count > 255) return -1;
    ata_select(lba, count, ATA_CMD_READ);
    if (ata_wait_ready() < 0) return -1;
    if (!(inb(ATA_STATUS) & ATA_SR_DRQ)) return -1;
    uint16_t *p = (uint16_t *)buf;
    for (uint32_t i = 0; i < count * 256u; i++) p[i] = inw(ATA_DATA);
    if (ata_wait_ready() < 0) return -1;   /* 等传输收尾 */
    return 0;
}

int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf) {
    if (!disk_present || count == 0 || count > 255) return -1;
    ata_select(lba, count, ATA_CMD_WRITE);
    if (ata_wait_ready() < 0) return -1;
    uint16_t *p = (uint16_t *)buf;
    for (uint32_t i = 0; i < count * 256u; i++) outw(ATA_DATA, p[i]);
    if (ata_wait_ready() < 0) return -1;   /* 等落盘完成 */
    if (inb(ATA_STATUS) & ATA_SR_ERR) return -1;
    return 0;
}

uint32_t ata_sectors(void) { return disk_sectors; }

int ata_init(void) {
    outb(ATA_CTRL_BASE, 0x0Au);   /* 关中断（nIEN=1），纯轮询 */
    outb(ATA_DRIVE, 0xA0u);       /* 选 master，LBA=1 */
    outb(ATA_CMD, ATA_CMD_IDENTIFY);

    uint8_t st = inb(ATA_STATUS);
    if (st == 0x00u || st == 0xFFu) {   /* 无设备（总线悬空/未响应） */
        serial_puts("[ata] no IDE disk on primary master\n");
        return 0;
    }
    if (ata_wait_ready() < 0) return 0;          /* 超时/ERR（可能 ATAPI） */
    if (!(inb(ATA_STATUS) & ATA_SR_DRQ)) return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    if (id[0] & 0x8000u) return 0;               /* ATAPI（光驱等）不支持 */

    disk_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);   /* LBA28 扇区数 */
    if (disk_sectors == 0) disk_sectors = 2048;   /* 兜底：1MB */
    disk_present = 1;
    serial_printf("[ata] IDE disk: %u sectors (%u KB), LBA28 PIO\n",
                  disk_sectors, disk_sectors / 2);
    return 1;
}
