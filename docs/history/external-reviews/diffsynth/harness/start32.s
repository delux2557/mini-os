/* harness/start32.s — 32 位 freestanding 入口与系统调用薄封装。
 * 注意：所有手写 asm 必须遵守 cdecl 被调用者保存约定（%ebx/%esi/%edi/%ebp 需还原），
 * 否则上层 gcc 代码会把活跃变量放这些寄存器里而被悄悄打飞（本 harness 曾踩）。 */
    .text
    .globl _start
_start:
    movl (%esp), %eax        /* argc */
    movl %esp, %ebx
    addl $4, %ebx            /* argv */
    pushl %ebx
    pushl %eax
    call crt_main
    movl %eax, %ebx
    movl $1, %eax
    int $0x80
    hlt

/* mmap2(addr,len,prot,flags,fd,pgoff) —— int $0x80 的 6 参约定 */
    .globl mmap2x
mmap2x:
    pushl %ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 20(%esp), %ebx
    movl 24(%esp), %ecx
    movl 28(%esp), %edx
    movl 32(%esp), %esi
    movl 36(%esp), %edi
    movl 40(%esp), %ebp
    movl $192, %eax
    int $0x80
    popl %ebx
    popl %edi
    popl %esi
    popl %ebp
    ret

/* enter_prod(eip, esp)：切栈后跳入产物，不返回 */
    .globl enter_prod
enter_prod:
    movl 4(%esp), %eax
    movl 8(%esp), %esp
    jmp *%eax
    hlt
