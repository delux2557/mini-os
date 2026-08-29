/* mini-os/v2-c-kernel/src/pci.h
 * PCI 总线访问（v0.18）：type-1 配置空间读写（0xCF8/0xCFC）。
 * 用于在启动时定位 e1000 网卡并分配其 MMIO BAR（QEMU -kernel 不经 BIOS，
 * BAR 未预分配，须由内核写入）。
 */
#ifndef _PCI_H
#define _PCI_H
#include <stdint.h>

/* 读/写某设备配置空间（off 须 4 字节对齐） */
uint32_t pci_config_read (uint32_t bus, uint32_t dev, uint32_t func, uint32_t off);
void     pci_config_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t off, uint32_t val);

/* 扫描总线 0，找 vendor/device 匹配的第一个设备；成功返回 1 并填出 bus/dev/func */
int pci_find(uint16_t vendor, uint16_t device,
             uint32_t *bus, uint32_t *dev, uint32_t *func);

/* 为该设备的 32 位内存 BAR 分配地址（探测大小 -> 写到 PCI MMIO 洞），
 * 并置位 command 的 MEM|BUSMASTER；返回分配到的基址（失败 0）。 */
uint32_t pci_bar_alloc_mem(uint32_t bus, uint32_t dev, uint32_t func, uint32_t bar_off);

#endif
