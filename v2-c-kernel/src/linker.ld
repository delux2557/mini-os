/* mini-os/v2-c-kernel/linker.ld
 * 把内核链接到 1MB 处（x86 传统约定），multiboot 头放在最前 */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    _kernel_start = .;

    .multiboot ALIGN(4K) : { *(.multiboot) }
    .text      ALIGN(4K) : { *(.text) }
    .rodata    ALIGN(4K) : { *(.rodata) }
    .data      ALIGN(4K) : { *(.data) }
    .bss       ALIGN(4K) : { *(COMMON) *(.bss) }

    _kernel_end = ALIGN(4K);
}
