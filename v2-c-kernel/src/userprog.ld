/* mini-os/v2-c-kernel/userprog.ld
 * 用户程序布局：固定链接到 0x80000000。
 * .text.entry(user_main_a) 必须放在最前，使 binary 从 0x80000000 起即入口。
 * .text.b(user_main_b)、.text.sem(user_main_sa/sb)、.text.msg(user_msg_p/c)、
 * .crash(user_crash) 的偏移由 Makefile 用 nm 提取。 */
ENTRY(user_main_a)

SECTIONS
{
    . = 0x80000000;
    .text.entry : { *(.text.entry) }
    .text.b      : { *(.text.b) }
    .text.sem    : { *(.text.sem) }
    .text.msg    : { *(.text.msg) }
    .text.fs     : { *(.text.fs) }
    .text        : { *(.text) }
    .crash ALIGN(16) : { *(.crash) }
    .rodata      : { *(.rodata) }
    .data        : { *(.data) }
    .bss         : { *(.bss) }
}
