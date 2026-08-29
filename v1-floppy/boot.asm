;==============================================================
; mini-os/v1-floppy/boot.asm
; 一个最小的 x86 引导扇区（512 字节）：
;   实模式 -> 开 A20 -> 进入保护模式 -> 打印到 VGA 与串口(COM1)
; 编译: nasm -f bin boot.asm -o boot.bin
; 测试: qemu-system-i386 -fda os.img -display none -serial file:serial.log
;==============================================================

[org 0x7c00]
[bits 16]

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    cli
    lgdt [gdt_descriptor]

    ; 打开 A20 地址线（8042 方法，QEMU 兼容）
    in al, 0x92
    or al, 0x02
    out 0x92, al

    ; 置位 CR0 保护模式位
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax

    ; 远跳转刷新流水线，载入代码段选择子 0x08
    jmp 0x08:protected_mode

[bits 32]
protected_mode:
    ; 载入数据段选择子 0x10
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    call init_serial

    ; 清屏（VGA 文本模式，填黑底白字空格）
    mov edi, 0xB8000
    mov ecx, 2000          ; 80 * 25
    mov eax, 0x0720
    rep stosw

    ; 第 1 行：白字
    mov edi, 0xB8000
    mov esi, msg1
    mov ah, 0x0F
    call print
    call serial_newline

    ; 第 2 行：绿字
    mov edi, 0xB8000 + 160
    mov esi, msg2
    mov ah, 0x0A
    call print
    call serial_newline

.hang:
    cli
    hlt
    jmp .hang

;----------------------------------------------
; print: 同时输出到 VGA 与串口
;   esi = 字符串地址, ah = VGA 属性, edi = VGA 目标
;----------------------------------------------
print:
    push eax
    push edi
.loop:
    lodsb
    test al, al
    jz .ret
    mov [edi], ax
    add edi, 2
    push ax
    call serial_write
    pop ax
    jmp .loop
.ret:
    pop edi
    pop eax
    ret

;----------------------------------------------
; serial_newline: 向串口输出 CR LF
;----------------------------------------------
serial_newline:
    mov al, 0x0d
    call serial_write
    mov al, 0x0a
    call serial_write
    ret

;----------------------------------------------
; init_serial: 初始化 COM1 (0x3F8) 为 38400 8N1
;----------------------------------------------
init_serial:
    push ax
    push dx
    mov dx, 0x3F9
    mov al, 0x00          ; 关闭中断
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80          ; DLAB = 1
    out dx, al
    mov dx, 0x3F8
    mov al, 0x03          ; 波特率分频低位（38400）
    out dx, al
    mov dx, 0x3F9
    mov al, 0x00          ; 高位
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03          ; 8 数据位、无校验、1 停止位
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    pop dx
    pop ax
    ret

;----------------------------------------------
; serial_write: 向 COM1 发送一个字符 (al)
;----------------------------------------------
serial_write:
    push dx
    push ax
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20         ; 发送缓冲为空？
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    pop dx
    ret

msg1 db "Hello Micro-OS! (protected mode)", 0
msg2 db "Booted via QEMU - written by an AI agent", 0

;----------------------------------------------
; GDT: 空描述符 / 代码段(ring0,32位,4K粒度) / 数据段
;----------------------------------------------
gdt_start:
    dq 0x0000000000000000
gdt_code:
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
gdt_data:
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times 510 - ($ - $$) db 0
dw 0xAA55
