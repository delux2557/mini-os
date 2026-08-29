/* mini-os/v2-c-kernel/src/pci.c
 * PCI type-1 配置空间访问（v0.18）。
 * QEMU `-kernel` 引导不经 SeaBIOS，PCI BAR 不会预分配：驱动须自行探测大小、
 * 在 PCI MMIO 洞（约 0xFEB00000 起）分配地址并写回 BAR，再使能 MEM|BUSMASTER。
 */
#include "pci.h"
#include "serial.h"

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static uint32_t pci_addr(uint32_t bus, uint32_t dev, uint32_t func, uint32_t off) {
    return 0x80000000u | (bus << 16) | (dev << 11) | (func << 8) | (off & 0xFCu);
}

uint32_t pci_config_read(uint32_t bus, uint32_t dev, uint32_t func, uint32_t off) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}
void pci_config_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t off, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, val);
}

int pci_find(uint16_t vendor, uint16_t device,
             uint32_t *bus, uint32_t *dev, uint32_t *func) {
    for (uint32_t d = 0; d < 32; d++) {
        for (uint32_t f = 0; f < 8; f++) {
            uint32_t id = pci_config_read(0, d, f, 0);
            if (id == 0xFFFFFFFFu) {           /* 无设备/无功能 */
                if (f == 0) break;
                continue;
            }
            if ((uint16_t)(id & 0xFFFF) == vendor && (uint16_t)(id >> 16) == device) {
                *bus = 0; *dev = d; *func = f;
                return 1;
            }
            if (f == 0 && !(pci_config_read(0, d, 0, 0x0C) & 0x80u)) break;  /* 单功能 */
        }
    }
    return 0;
}

uint32_t pci_bar_alloc_mem(uint32_t bus, uint32_t dev, uint32_t func, uint32_t bar_off) {
    uint32_t bar = pci_config_read(bus, dev, func, bar_off);
    uint32_t lo = bar & 0xFu;                       /* 保留低位标志位 */
    if ((bar & 0xFFFFFFF0u) != 0) {                 /* 已预分配（保留原地址） */
        uint32_t addr = bar & 0xFFFFFFF0u;
        /* 仍确保 command 打开 MEM|BUSMASTER */
        uint32_t cmd = pci_config_read(bus, dev, func, 0x04);
        pci_config_write(bus, dev, func, 0x04, cmd | 0x6u);
        return addr;
    }
    /* 探测 BAR 大小：全 1 写回再读（32 位内存 BAR） */
    pci_config_write(bus, dev, func, bar_off, 0xFFFFFFFFu);
    uint32_t szreg = pci_config_read(bus, dev, func, bar_off);
    pci_config_write(bus, dev, func, bar_off, 0);
    uint32_t size = (~(szreg & 0xFFFFFFF0u)) + 1u;  /* 2 的幂 */
    if (size == 0) return 0;
    /* 分配到 PCI MMIO 洞：0xFEB00000 起（> 64MB RAM，避开恒等映射区与设备） */
    static uint32_t mmio_hint = 0xFEB00000u;
    uint32_t addr = mmio_hint;
    mmio_hint += size;
    pci_config_write(bus, dev, func, bar_off, (addr & 0xFFFFFFF0u) | lo);
    /* 使能 memory + bus master（DMA 收发环需要） */
    uint32_t cmd = pci_config_read(bus, dev, func, 0x04);
    pci_config_write(bus, dev, func, 0x04, cmd | 0x6u);
    serial_printf("[pci] BAR%u -> %x (size %x)\n", bar_off / 4, addr, size);
    return addr;
}
