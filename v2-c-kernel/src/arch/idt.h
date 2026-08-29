/* mini-os/v2-c-kernel/idt.h */
#ifndef _IDT_H
#define _IDT_H
#include <stdint.h>

/* 中断发生时由 isr_common_stub 压入的寄存器现场 */
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, user_esp, ss;
} registers_t;

typedef void (*isr_t)(registers_t *);

void idt_init(void);
/* 注册 IRQ 处理函数（0..15），用于定时器/键盘等外设中断 */
void irq_install_handler(int irq, isr_t handler);

#endif
