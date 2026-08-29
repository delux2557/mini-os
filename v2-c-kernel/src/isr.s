;==============================================================
; mini-os/v2-c-kernel/isr.s
; 48 个中断入口桩（32 异常 + 16 IRQ）+ 统一分发例程
; 每个桩压入（错误码，中断号）后跳到 isr_common_stub
;==============================================================

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push dword 0        ; 伪错误码
    push dword %1       ; 中断号
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push dword %1       ; CPU 已压入错误码
    jmp isr_common_stub
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp isr_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; 系统调用：int 0x80（用户态可通过 DPL=3 的中断门进入）
ISR_NOERRCODE 128

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

extern isr_handler

isr_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp            ; 传 registers_t* 给 C
    call isr_handler
    pop eax             ; 丢弃参数（此后 esp 指回 gs 槽）

;==============================================================
; resume_point：进程/中断现场恢复点。
; 调度器切换进程时，把 esp 指向目标进程的 gs 槽，ret 到这里，
; 即可用目标进程保存的现场继续执行（与中断正常返回路径一致）。
;==============================================================
global resume_point
resume_point:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; 清理 中断号 + 错误码
    iret

;==============================================================
; sched_switch_esp(uint32_t kernel_esp)：无条件切到目标进程的
; gs 槽地址并跳进 resume_point 恢复现场（不返回）。
; 不能依赖 [esp-4] 里放返回地址：预占帧的 [esp-4] 是 push esp
; 压入的 gs 指针，不是代码地址；统一改为 esp=gs槽 后直接 jmp。
;==============================================================
global sched_switch_esp
sched_switch_esp:
    mov eax, [esp + 4]  ; 参数：目标进程的 gs 槽地址（kernel_esp）
    mov esp, eax
    jmp resume_point

section .note.GNU-stack noalloc noexec nowrite progbits
