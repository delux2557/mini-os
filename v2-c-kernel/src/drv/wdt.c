/* mini-os/v2-c-kernel/src/drv/wdt.c
 * NMI 看门狗（Intel 6300ESB + QEMU -watchdog-action inject-nmi）。
 *
 * 背景（堆成环 BUG-074 复盘）：cli 段死循环时（如 kfree 链表成环），IF=0 屏蔽了
 * 定时器 IRQ，依赖 IRQ0 喂狗的软看门狗随之失效——CPU 已死循环且中断关闭，
 * 谁都进不去。NMI 不受 IF 影响，是唯一能突破该死循环的通道。
 *
 * 本驱动：使能 i6300esb 看门狗（QEMU 虚拟设备，PCI 0x8086/0x25AB），每 tick 喂狗；
 * 若宿主连续 ~6s 收不到喂狗（即 cli 死循环、IRQ0 不再到达），QEMU 按
 * -watchdog-action inject-nmi 向 vCPU 注入 NMI（向量 2）。nmi_handler 打印死循环
 * 现场后经键盘控制器软复位：系统自救并留下证据日志；-no-reboot 下 QEMU 随即退出。
 *
 * 寄存器语义以 QEMU hw/watchdog/wdt_i6300esb.c 为准（与 Linux i6300esb_wdt 驱动不同）：
 *   BAR0 为 16 字节"内存"BAR（非 IO 空间；QEMU pci_register_bar type=0 是 MEM）。
 *     0x00/0x04  timer1/timer2 预装载值（双字写，须先解锁；1KHz 时钟下 1 tick≈0.983ms）
 *     0x0C       RELOAD：字节 0x80→0x86 解锁；解锁后字写 bit8=1 即喂狗(ping)
 *   PCI config：
 *     0x60（字）  时钟/中断配置（bit2=1 切 1MHz，默认 1KHz；bit5=1 关闭超时重启）
 *     0x68（字节） bit1=1 使能（0→1 沿启动计时）
 *   stage1 超时后进入 stage2，stage2 超时才执行 -watchdog-action（两段合计为总超时）。
 *   喂狗(ping) 只重置 stage1，故 stage2 永不触发——除非整个时钟停摆（cli 死循环）。
 */
#include "idt.h"
#include "mem.h"
#include "pci.h"
#include "serial.h"
#include <stdint.h>

#define WDT_PCI_VENDOR 0x8086u
#define WDT_PCI_DEVICE 0x25ABu

#define ESB_TIMER1_REG 0x00u
#define ESB_TIMER2_REG 0x04u
#define ESB_RELOAD_REG 0x0Cu
#define ESB_UNLOCK1    0x80u
#define ESB_UNLOCK2    0x86u
#define ESB_WDT_PING   0x0100u   /* 喂狗：解锁后字写 RELOAD bit8=1 */

#define ESB_CONFIG_REG 0x60u     /* 字：bit2=1 时钟 1MHz；bit1:0=int_type；bit5=1 关重启 */
#define ESB_LOCK_REG   0x68u     /* 字节：bit1=1 使能 */
#define ESB_WDT_ENABLE 0x02u

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

static volatile uint8_t *wdt_base = 0;   /* MMIO 基址（恒等映射一页后） */
static int wdt_ok = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static uint32_t pci_addr(uint32_t bus, uint32_t dev, uint32_t func, uint32_t off) {
    return 0x80000000u | (bus << 16) | (dev << 11) | (func << 8) | (off & 0xFCu);
}

/* config 0x60/0x68 是字/字节敏感的寄存器，须按对应宽度写
 * （32 位写会落入 pci_default_write_config，QEMU 侧不生效）。 */
static void pci_config_writeb(uint32_t bus, uint32_t dev, uint32_t func,
                              uint32_t off, uint8_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, off));
    outb(PCI_CONFIG_DATA, val);
}
static void pci_config_writew(uint32_t bus, uint32_t dev, uint32_t func,
                              uint32_t off, uint16_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, off));
    outw(PCI_CONFIG_DATA, val);
}

static void esb_unlock(void) {
    wdt_base[ESB_RELOAD_REG] = ESB_UNLOCK1;
    wdt_base[ESB_RELOAD_REG] = ESB_UNLOCK2;
}

/* 喂狗：解锁后字写 RELOAD bit8=1 → QEMU 重置 stage1 计时。
 * MMIO 基址落在 PDE>=512（高地址），用户页目录只克隆低 1GB PDE，凡访问
 * 须临时切到内核页目录（与 e1000_netif 的 enter_kernel_pd 同款；IRQ 上下文
 * 切换 CR3 是该内核既有模式，DHCP 原子能力同样在 syscall 上下文切换）。 */
void wdt_feed(void) {
    if (!wdt_ok) return;
    uint32_t saved = mem_current_pd();
    if (saved != mem_kernel_pd()) switch_page_dir(mem_kernel_pd());
    esb_unlock();
    *(volatile uint16_t *)(wdt_base + ESB_RELOAD_REG) = ESB_WDT_PING;
    if (saved != mem_kernel_pd()) switch_page_dir(saved);
}

void wdt_init(void) {
    uint32_t bus = 0, dev = 0, func = 0;
    uint32_t bar;

    if (!pci_find(WDT_PCI_VENDOR, WDT_PCI_DEVICE, &bus, &dev, &func)) {
        serial_puts("[wdt] i6300esb not found, NMI watchdog disabled\n");
        return;
    }
    /* BAR0 是内存 BAR（16 字节）。pci_bar_alloc_mem：SeaBIOS 已预分配则保留原地址，
     * 未预分配则自分配，并置 MEM|BUSMASTER。 */
    bar = pci_bar_alloc_mem(bus, dev, func, 0x10);
    if (!bar) { serial_puts("[wdt] i6300esb no MMIO BAR\n"); return; }
    /* 恒等映射 MMIO 页到内核页目录（与 e1000 同款；低半区 PDE 被所有进程克隆） */
    if (map_page_in(mem_kernel_pd(), bar & ~0xFFFu, bar & ~0xFFFu, 0x3) != 0) {
        serial_puts("[wdt] i6300esb MMIO map failed, disabled\n");
        return;
    }
    wdt_base = (volatile uint8_t *)(unsigned long)bar;

    /* 时钟保持默认 1KHz（1 tick ≈ 0.983ms）；int_type=IRQ(0)；默认 reboot_enabled=1。
     * stage1(0x1400≈5.03s) + stage2(0x0400≈1.01s) 总超时 ≈ 6s。
     * 注意：QEMU 每次解锁状态只维持到下一个非解锁写（写完即复位为 0），
     * 故两个预装载寄存器各须一次完整解锁。 */
    pci_config_writew(bus, dev, func, ESB_CONFIG_REG, 0x0000);

    esb_unlock();
    *(volatile uint32_t *)(wdt_base + ESB_TIMER1_REG) = 0x1400u;
    esb_unlock();
    *(volatile uint32_t *)(wdt_base + ESB_TIMER2_REG) = 0x0400u;

    /* 使能：lock 寄存器字节写 bit1=1（0→1 沿启动计时） */
    pci_config_writeb(bus, dev, func, ESB_LOCK_REG, ESB_WDT_ENABLE);

    wdt_ok = 1;
    serial_printf("[wdt] i6300esb enabled mmio=%x timeout~6s NMI watchdog armed\n",
                  bar);
}

/* ---- NMI（向量 2）处理：QEMU 按 -watchdog-action inject-nmi 注入 ----
 * cli 死循环里定时器中断被屏蔽、软看门狗失效，唯 NMI 能进入。打印完整现场
 * （寄存器 + 调用栈回溯 + 符号，eflags 可见 IF=0 铁证）后经键盘控制器软复位。 */
void nmi_handler(registers_t *r) {
    extern void panic_dump(registers_t *);

    serial_puts("\n=========== NMI WATCHDOG ===========\n");
    serial_printf("[wdt] NMI fired (soft wdt failed) IF=%u cs=%x eip=%x\n",
                  (r->eflags >> 9) & 1u, r->cs, r->eip);
    panic_dump(r);
    serial_puts("[wdt] deadlock detected, soft reset...\n");
    outb(0x64, 0xFE);              /* 键盘控制器复位（QEMU 实现为整机 reset） */
    __asm__ volatile ("cli; hlt"); /* 复位失败兜底 */
}
