;==============================================================
; mini-os/v2-c-kernel/boot.s
; multiboot 头 + 内核入口（被 QEMU -kernel 或 GRUB 加载）
; 1) 声明 multiboot 头（magic/flags/checksum）
; 2) 自建平坦 GDT，脱离对引导器的依赖
; 3) 设置栈，把 multiboot 参数传给 C 的 kernel_main
;==============================================================

section .multiboot
align 4
    dd 0x1BADB002              ; magic
    dd 0x00000003              ; flags: 页 4KB 对齐 + 提供内存信息
    dd -(0x1BADB002 + 0x00000003) ; checksum

section .text
global _start
_start:
    cli
    lgdt [gdt_desc]
    jmp 0x08:.reload
.reload:
    ; 先设栈，并把引导器传入的 EAX(magic)/EBX(info) 保存，
    ; 因为下面 mov ax,0x10 会破坏 EAX 低位
    mov esp, stack_top
    push ebx                  ; 保存 multiboot info
    push eax                  ; 保存 magic

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; 栈顶 = magic(EAX)，其上 = info(EBX) → kernel_main(magic, info)
    extern kernel_main
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

section .rodata
align 8
gdt_start:
    dq 0x0000000000000000
gdt_code:
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
gdt_data:
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

section .bss
align 16
stack_bottom:
    resb 16384
global stack_top
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
