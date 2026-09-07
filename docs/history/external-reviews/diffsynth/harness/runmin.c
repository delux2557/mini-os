/* harness/runmin.c — 评估宿主：在 Linux/i386 上直接运行 minicc 产物（mini-os ELF）。
 *
 * 背景：minicc 产物是 PT_LOAD@0x800A0000、p_offset=0 的裸 ELF，入口 0x54 固定为
 *   `e8 rel32(call main) | 89 c3(mov eax,ebx) | 31 c0(xor eax,eax) | cd 80(int 0x80)`
 *   —— int $0x80 的号是 mini-os 契约，Linux 无法执行。为在无 qemu-system-i386 的环境
 *   做"运行语义差分"，本 runner 把镜像 MAP_FIXED 到 0x800A0000 后做定点补丁：
 *     1) 入口 stub 0x59 的 7 字节 → `e9 rel32 exit_bridge`+`90`（exit_bridge: mov eax,ebx;
 *        mov $1,eax; int $0x80 = Linux exit(code)，与 gcc 参考程序同口径比退出码）；
 *     2) syscall3 stub（按已知字节型样式识别）的 `cd 80 5d c3` → `e9 rel32 os_bridge`+`90 90`，
 *        os_bridge 按 mini-os 号分发到宿主（1=print→write, 13..19=宿主文件, 35=brk, 0=exit），
 *        并自行补回被覆盖的 `pop %ebp; ret`。
 *   仅"评估宿主"，不改被评对象一行代码。同时校验 ELF 结构，头被写坏时如实报告。
 * 用法：runmin <elf> | runmin --check <elf>
 */
#include <stdint.h>

extern long do_mmap(unsigned addr, unsigned len, unsigned prot, unsigned flags, int fd);
extern long mmap2x(unsigned addr, unsigned len, unsigned prot, unsigned flags, int fd, unsigned pgoff);
extern void enter_prod(void *eip, void *esp);
extern void exit_bridge(void), os_bridge(void);

static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}
static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void emit(const char *s) { sys3(4, 2, (long)s, my_strlen(s)); }
static void emit_out(const char *s) { sys3(4, 1, (long)s, my_strlen(s)); }
static void hexout(unsigned v) {
    static const char d[] = "0123456789abcdef";
    char b[9];
    for (int i = 0; i < 8; i++) b[i] = d[(v >> ((7 - i) * 4)) & 15];
    b[8] = 0; emit(b);
}
static void decout(int v) {
    char b[12]; int i = 9; b[10] = 0;
    if (v < 0) { emit("-"); v = -v; }
    do { b[i--] = (char)('0' + v % 10); v /= 10; } while (v);
    emit(b + i + 1);
}

/* ---- mini-os syscall 契约的宿主实现（语义对齐仓库 tools/minicc/host_crt.c） ---- */
static int sfd[8];
int os_syscall_reorder(int n, int a, int b, int c) {
    switch (n) {
    case 0:  sys3(1, a & 0xff, 0, 0); for (;;) {}
    case 1:  return (int)sys3(4, 1, (long)(uintptr_t)a, my_strlen((const char *)(uintptr_t)a));
    case 13: {
        int fd = (int)sys3(5, (long)(uintptr_t)a, 2 | 0x40 | 0x200, 0644);
        if (fd < 0) return -1;
        sys3(6, fd, 0, 0); return 0;
    }
    case 14: {
        if (a <= 0 || a >= 8 || sfd[a]) return -1;
        int fd = (int)sys3(5, (long)(uintptr_t)b, c ? (2 | 0x40) : 0, 0644);
        if (fd < 0) return -1;
        sfd[a] = fd; return 0;
    }
    case 15: if (a <= 0 || a >= 8 || !sfd[a]) return -1;
             return (int)sys3(4, sfd[a], (long)(uintptr_t)b, c);
    case 16: if (a <= 0 || a >= 8 || !sfd[a]) return -1;
             return (int)sys3(3, sfd[a], (long)(uintptr_t)b, c);
    case 17: if (a > 0 && a < 8 && sfd[a]) { sys3(6, sfd[a], 0, 0); sfd[a] = 0; } return 0;
    case 19: return sys3(10, (long)(uintptr_t)a, 0, 0) == 0 ? 0 : -1;
    case 35: if (a == 0) return (int)sys3(45, 0, 0, 0);
             { long r = sys3(45, (uintptr_t)a, 0, 0); return r == (long)(uintptr_t)a ? 0 : -1; }
    default: return -1;
    }
}

/* bridge：exit_bridge 收 main 返回值；os_bridge 收 mini-os 号 + 参数（见文件头） */
__asm__(
    ".text\n"
    ".globl exit_bridge\n"
    "exit_bridge:\n"
    "  movl %eax,%ebx\n  movl $1,%eax\n  int $0x80\n  hlt\n"
    ".globl os_bridge\n"
    "os_bridge:\n"
    "  pushl %edi\n  pushl %esi\n  pushl %ebp\n"
    "  pushl %edx\n  pushl %ecx\n  pushl %ebx\n  pushl %eax\n"
    "  subl $16,%esp\n"
    "  movl 16(%esp),%eax\n  movl %eax,0(%esp)\n"
    "  movl 20(%esp),%eax\n  movl %eax,4(%esp)\n"
    "  movl 24(%esp),%eax\n  movl %eax,8(%esp)\n"
    "  movl 28(%esp),%eax\n  movl %eax,12(%esp)\n"
    "  call os_syscall_reorder\n"
    "  movl %eax,16(%esp)\n"
    "  addl $16,%esp\n"
    "  popl %eax\n  popl %ebx\n  popl %ecx\n  popl %edx\n"
    "  popl %ebp\n  popl %esi\n  popl %edi\n"
    "  popl %ebp\n  ret\n");

#define BASE 0x800A0000u
static unsigned char img[1 << 19];
static int rd4(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (int)((unsigned)p[3] << 24); }
static void wr4(unsigned char *p, int v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = (unsigned)v >> 24; }
static void die(const char *m, int v) { emit("[runner] "); emit(m); decout(v); emit("\n"); sys3(1, 3, 0, 0); for (;;) {} }

int crt_main(int argc, char **argv) {
    int check_only = 0, i0 = 1;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') { check_only = 1; i0 = 2; }
    if (argc < i0 + 1) die("usage: runmin [--check] <elf>\n", 2);

    int fd = (int)sys3(5, (long)argv[i0], 0, 0);
    if (fd < 0) die("open fail ", 1);
    int len = 0, r;
    while ((r = (int)sys3(3, fd, (long)(img + len), 4096)) > 0) {
        len += r;
        if (len >= (1 << 19)) die("too big", 3);
    }
    sys3(6, fd, 0, 0);
    if (len < 95) die("short file ", len);

    int bad = 0;
    if (!(img[0] == 0x7f && img[1] == 'E' && img[2] == 'L' && img[3] == 'F')) {
        emit("[runner] ELF MAGIC BROKEN bytes0-7:");
        for (int i = 0; i < 8; i++) { emit(" "); hexout(img[i]); }
        emit("\n"); bad = 1;
    }
    if (img[4] != 1 || img[5] != 1) {
        emit("[runner] EI_CLASS/DATA BROKEN: "); hexout(img[4]); emit(" "); hexout(img[5]); emit("\n"); bad = 1;
    }
    if (rd4(img + 28) != 52) { emit("[runner] e_phoff bad\n"); bad = 1; }
    if (rd4(img + 52) != 1) { emit("[runner] p_type != PT_LOAD\n"); bad = 1; }
    if (rd4(img + 68) != len) { emit("[runner] p_filesz!=size\n"); bad = 1; }
    if (rd4(img + 72) != rd4(img + 68)) { emit("[runner] p_memsz!=filesz\n"); bad = 1; }
    if ((unsigned)rd4(img + 60) != BASE) { emit("[runner] p_vaddr bad\n"); bad = 1; }
    if (check_only) { emit_out(bad ? "[check] BAD\n" : "[check] OK\n"); return bad ? 1 : 0; }
    if (bad) {
        img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
        img[4] = 1; img[5] = 1; img[6] = 1;
        emit("[runner] header repaired locally (mini-os elf_load would reject) \n");
    }
    if (img[0x54] != 0xe8) die("no call-main stub at 0x54", 4);
    if (!(img[0x59] == 0x89 && img[0x5a] == 0xc3 && img[0x5b] == 0x31 &&
          img[0x5c] == 0xc0 && img[0x5d] == 0xcd && img[0x5e] == 0x80))
        die("exit stub mismatch at 0x59", 5);

    long m = mmap2x(BASE, (len + 4095) & ~4095, 7, 0x32, -1, 0);
    if (m != (long)BASE) die("mmap fail ", (int)m);
    unsigned char *lo = (unsigned char *)BASE;
    for (int i = 0; i < len; i++) lo[i] = img[i];

    lo[0x59] = 0xe9;
    wr4(lo + 0x5a, (int)((uintptr_t)&exit_bridge - (BASE + 0x59 + 5)));
    lo[0x5e] = 0x90;

    static const unsigned char pat[16] = {
        0x55,0x89,0xe5,0x8b,0x45,0x14,0x8b,0x5d,0x10,0x8b,0x4d,0x0c,0x8b,0x55,0x08,0xcd };
    for (int i = 0x5f; i + 18 <= len; i++) {
        int hit = 1;
        for (int j = 0; j < 16; j++) if (lo[i + j] != pat[j]) { hit = 0; break; }
        if (!hit || lo[i + 16] != 0x80) continue;
        lo[i + 15] = 0xe9;
        wr4(lo + i + 16, (int)((uintptr_t)&os_bridge - (BASE + i + 15 + 5)));
            i += 17;
    }

    if (argc > i0 + 1) {   /* 任何额外参数 = noexec 调试模式 */
        emit(" os_bridge="); hexout((unsigned)(uintptr_t)&os_bridge); emit(" len="); decout(len); emit("\n");
        return 0;
    }
    long st = mmap2x(0x60000000u, 8u << 20, 3, 0x32, -1, 0);
    if (st != 0x60000000) die("stack mmap fail ", (int)st);
    enter_prod((void *)(BASE + 0x54), (void *)(0x60000000u + (8u << 20) - 4096));
    die("product returned?", 6);
    return 9;
}
