/* mini-os/v2-c-kernel/idt.c
 * 中断系统：
 *  - IDT：32 个 CPU 异常 + 16 个硬件 IRQ（重映射到 0x20~0x2F）
 *  - PIC 8259 初始化
 *  - 统一入口 isr_handler 分发到各 IRQ 的 C 处理函数 */
#include "idt.h"
#include "serial.h"
#include "vga.h"
#include <stdint.h>

struct idt_entry {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr  idtp;
static isr_t irq_handlers[16];

/* ---- 用 X-macro 展开 48 个汇编入口桩的声明与表 ---- */
#define ISR_LIST \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) \
    X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) \
    X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
#define IRQ_LIST \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) \
    X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)

#define X(n) extern void isr##n(void);
ISR_LIST
#undef X
extern void isr128(void);        /* int 0x80 系统调用入口 */

#define X(n) extern void irq##n(void);
IRQ_LIST
#undef X

#define X(n) isr##n,
static void (*isr_funcs[32])(void) = { ISR_LIST };
#undef X

#define X(n) irq##n,
static void (*irq_funcs[16])(void) = { IRQ_LIST };
#undef X

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void set_idt_gate(uint8_t n, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[n].base_lo = (uint16_t)(base & 0xFFFF);
    idt[n].base_hi = (uint16_t)((base >> 16) & 0xFFFF);
    idt[n].sel  = sel;
    idt[n].zero = 0;
    idt[n].flags = flags; /* 0x8E = present, 32 位中断门 */
}

/* 8259 重映射：主片 0x20~0x27，从片 0x28~0x2F */
static void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    /* 掩码：只允许 IRQ0(定时器)、IRQ1(键盘) 与 IRQ4(串口 COM1 接收) */
    outb(0x21, 0xEC);
    outb(0xA1, 0xFF);
}

void irq_install_handler(int irq, isr_t handler) {
    if (irq >= 0 && irq < 16) irq_handlers[irq] = handler;
}

/* 汇编桩调用进来的统一入口（在 isr.s 中定义） */
void isr_handler(registers_t *r) {
    /* L0（栈预算总账）：先校验当前进程内核栈底部 canary。中断门进即关 IF、单链独占
     * 4KB 内核栈，上一次 IRQ/syscall 深链若写穿栈底，这里立即检出并停机（SEC-07）。 */
    extern void kstack_check(void);
    kstack_check();
    if (r->int_no >= 32 && r->int_no < 48) {
        int irq = (int)(r->int_no - 32);
        /* EOI 必须在调用处理函数之前发出：
         * 定时器处理函数会切换进程（不再返回到这里） */
        if (irq >= 8) outb(0xA0, 0x20); /* 从片 EOI */
        outb(0x20, 0x20);              /* 主片 EOI */
        if (irq_handlers[irq]) irq_handlers[irq](r);
    } else if (r->int_no == 14) {
        /* 页错误：交由内存管理处理，懒分配区内可恢复重试 */
        extern void pf_handler(registers_t *);
        pf_handler(r);
    } else if (r->int_no == 128) {
        /* 系统调用：int 0x80 */
        extern void syscall_dispatch(registers_t *);
        syscall_dispatch(r);
    } else {
        /* CPU 异常：先打印完整现场（寄存器 + 调用栈回溯 + 符号化）。SEC-01 修复：区分来源——
         * 用户态异常（#DE/#UD/#GP/#BP/#AC…）与 pf_handler 同构，杀进程并调度，
         * 不让低权限 ring3 程序借一条 ud2/除零/cli 使整机停机（DoS）；
         * 仅内核态异常说明内核自身缺陷、无法安全继续，才 cli;hlt 停机。 */
        extern void panic_dump(registers_t *);
        panic_dump(r);
        if ((r->cs & 3) == 3) {
            extern void sched_kill(registers_t *, uint32_t);
            extern uint32_t sched_current_pid(void);
            uint32_t pid = sched_current_pid();
            serial_printf("[user] CPU EXCEPTION #%u pid=%u eip=%x -> killed\n",
                          r->int_no, pid, r->eip);
            vga_printf("[user] CPU EXCEPTION #%u pid=%u eip=%x -> killed\n",
                       r->int_no, pid, r->eip);
            sched_kill(r, (uint32_t)-1);
            __asm__ volatile ("cli; hlt");   /* 不可达：sched_kill 已切换进程 */
        }
        __asm__ volatile ("cli; hlt");
    }
}

void idt_init(void) {
    int i;
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint32_t)&idt;

    for (i = 0; i < 32; i++) set_idt_gate((uint8_t)i, (uint32_t)isr_funcs[i], 0x08, 0x8E);
    for (i = 0; i < 16; i++) set_idt_gate((uint8_t)(32 + i), (uint32_t)irq_funcs[i], 0x08, 0x8E);
    /* 0x80 系统调用门：0xEE = present, DPL3（允许用户态触发）, 32 位中断门 */
    set_idt_gate(128, (uint32_t)isr128, 0x08, 0xEE);

    pic_remap();

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}
