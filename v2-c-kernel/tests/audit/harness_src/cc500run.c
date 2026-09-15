/* cc500run.c — 评估宿主：在 Linux/i386 上直接执行 cc500 产物（mini-os ELF）。
 * cc500 产物布局（tools/cc500/cc500.c be_start/be_finish）：
 *   0x00 ELF 头+phdr；0x54 入口 stub：
 *     8b 44 24 08 50 8b 44 24 08 50 e8 <rel32@0x5F>   ; mov eax,[esp+8];push;mov eax,[esp+8];push;call 首函数
 *     0x63: 89 c3 31 c0 cd 80                          ; mov %eax,%ebx; xor %eax,%eax; int $0x80
 *   0x69(105): syscall3 stub（19B）：8b 44 24 10 | 8b 5c 24 0c | 8b 4c 24 08 | 8b 54 24 04 | cd 80 | c3
 * 补丁：
 *   1) 0x63 的 6 字节 → `e9 rel32`+`90` → exit_bridge（Linux exit(产物退码)）；
 *   2) 105 起 5 字节 → `e9 rel32` → cc_bridge：按 [esp+16/12/8/4] 取 n/a/b/c 调宿主
 *      实现，返回后 `ret` 回调用方（参数由调用方 be_pop 清理，与 cc500 约定一致）。
 * 初始栈按内核 exec 契约布置：[esp]=0(fake ret) [esp+4]=argc [esp+8]=argv。
 * 另：shim 把以 '/' 开头的路径映射到 cwd（去掉前导 '/'），使默认 /cc500.c、/out.elf
 * 可用于自举 P1==P2 宿主复现。
 * 用法：cc500run [--check] <elf> [args...]   （args 透传给产物 argv[1..]）
 */
#include <stdint.h>

extern long mmap2x(unsigned addr, unsigned len, unsigned prot, unsigned flags, int fd, unsigned pgoff);
extern void enter_prod(void *eip, void *esp);
extern void exit_bridge(void), cc_bridge(void);

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

/* ---- mini-os syscall 契约的宿主实现（路径剥前导 '/' → cwd 语义） ---- */
static int sfd[8];
static const char *cpath(const char *p) { while (*p == '/') p++; return p; }
int cc_syscall_reorder(int n, int a, int b, int c) {
    emit("[svc]"); decout(n); emit(" a="); hexout((unsigned)a); emit(" b="); hexout((unsigned)b); emit(" c="); hexout((unsigned)c); emit("\n");
    switch (n) {
    case 0:  sys3(1, a & 0xff, 0, 0); for (;;) {}
    case 1:  return (int)sys3(4, 1, (long)(uintptr_t)a, my_strlen((const char *)(uintptr_t)a));
    case 13: {
        int fd = (int)sys3(5, (long)cpath((const char *)(uintptr_t)a), 2 | 0x40 | 0x200, 0644);
        if (fd < 0) return -1;
        sys3(6, fd, 0, 0); return 0;
    }
    case 14: {
        if (a <= 0 || a >= 8 || sfd[a]) return -1;
        int fd = (int)sys3(5, (long)cpath((const char *)(uintptr_t)b), c ? (2 | 0x40) : 0, 0644);
        if (fd < 0) return -1;
        sfd[a] = fd; return 0;
    }
    case 15: if (a <= 0 || a >= 8 || !sfd[a]) return -1;
             return (int)sys3(4, sfd[a], (long)(uintptr_t)b, c);
    case 16: if (a <= 0 || a >= 8 || !sfd[a]) return -1;
             return (int)sys3(3, sfd[a], (long)(uintptr_t)b, c);
    case 17: if (a > 0 && a < 8 && sfd[a]) { sys3(6, sfd[a], 0, 0); sfd[a] = 0; } return 0;
    case 19: return sys3(10, (long)cpath((const char *)(uintptr_t)a), 0, 0) == 0 ? 0 : -1;
    case 35: if (a == 0) return (int)sys3(45, 0, 0, 0);
             { long r = sys3(45, (uintptr_t)a, 0, 0); return r == (long)(uintptr_t)a ? 0 : -1; }
    default: return -1;
    }
}

/* cc_bridge：从 cc500 syscall3 stub 跳入。栈：[esp]=ret [esp+4]=c [esp+8]=b [esp+12]=a [esp+16]=n。
 * 服务完后 `ret` 回调用方；调用方压的 4 个参数由调用方 be_pop 清理（cc500 约定）。
 * eax = 返回值；ebx/ecx/edx 可破坏（cc500 约定 caller-saved，stub 本身就改写它们）。 */
__asm__(
    ".text\n"
    ".globl segv_restorer_asm\nsegv_restorer_asm:\n  movl $119,%eax\n  int $0x80\n"
    ".globl exit_bridge\n"
    "exit_bridge:\n"
    "  movl %eax,%ebx\n  movl $1,%eax\n  int $0x80\n  hlt\n"
    ".globl cc_bridge\n"
    "cc_bridge:\n"
    "  pushl %edi\n  pushl %esi\n  pushl %ebp\n"
    "  subl $16,%esp\n"
    "  movl 44(%esp),%eax\n  movl %eax,0(%esp)\n"   /* n */
    "  movl 40(%esp),%eax\n  movl %eax,4(%esp)\n"   /* a */
    "  movl 36(%esp),%eax\n  movl %eax,8(%esp)\n"   /* b */
    "  movl 32(%esp),%eax\n  movl %eax,12(%esp)\n"  /* c */
    "  call cc_syscall_reorder\n"
    "  addl $16,%esp\n"
    "  popl %ebp\n  popl %esi\n  popl %edi\n"
    "  ret\n");

/* SIGSEGV 诊断：打印出错 EIP/CR2（i386 sigcontext 布局） */
static struct { unsigned gs, fs, es, ds, edi, esi, ebp, esp, ebx, edx, ecx, eax, trapno, err, eip, cs, eflags, esp_at, ss, fpstate, oldmask, cr2; } *g_sc;
static void segv_handler(int sig) {
    (void)sig;
    emit("[cc500run] PRODUCT SEGV: eip="); hexout(g_sc->eip);
    emit(" cr2="); hexout(g_sc->cr2); emit("\n");
    sys3(1, 3, 0, 0);
    for (;;) {}
}
__asm__(".globl segv_restorer\nsegv_restorer:\n  movl $119,%eax\n  int $0x80\n");
extern void segv_restorer_asm(void);
static void install_segv(void) {
    struct { unsigned handler, flags, restorer, mask; } act;
    act.handler = (unsigned)segv_handler;
    act.flags = 0x04000000u | 0x40000000u;   /* SA_RESTORER | SA_RESTART */
    act.restorer = (unsigned)segv_restorer_asm;
    act.mask = 0;
    long r = sys3(174, (long)&act, 0, 8);    /* rt_sigaction(SIGSEGV=11) */
    (void)r;
}
#define BASE 0x800A0000u
static unsigned char img[1 << 19];
static int rd4(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (int)((unsigned)p[3] << 24); }
static void wr4(unsigned char *p, int v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = (unsigned)v >> 24; }
static void die(const char *m, int v) { emit("[cc500run] "); emit(m); decout(v); emit("\n"); sys3(1, 3, 0, 0); for (;;) {} }

int getenv_dbg = 0;
int crt_main(int argc, char **argv) {
    int check_only = 0, i0 = 1;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'd') { getenv_dbg = 1; i0 = 2; }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') { check_only = 1; i0 = 2; }
    if (argc < i0 + 1) die("usage: cc500run [--check] <elf> [args...]\n", 2);

    int fd = (int)sys3(5, (long)argv[i0], 0, 0);
    if (fd < 0) die("open fail ", 1);
    int len = 0, r;
    while ((r = (int)sys3(3, fd, (long)(img + len), 4096)) > 0) {
        len += r;
        if (len >= (1 << 19)) die("too big", 3);
    }
    sys3(6, fd, 0, 0);
    if (len < 105) die("short file ", len);

    int bad = 0;
    if (!(img[0] == 0x7f && img[1] == 'E' && img[2] == 'L' && img[3] == 'F')) {
        emit("[cc500run] ELF MAGIC BROKEN bytes0-7:");
        for (int i = 0; i < 8; i++) { emit(" "); hexout(img[i]); }
        emit("\n"); bad = 1;
    }
    if (img[4] != 1 || img[5] != 1) { emit("[cc500run] EI_CLASS/DATA BROKEN\n"); bad = 1; }
    if (rd4(img + 28) != 52) { emit("[cc500run] e_phoff bad\n"); bad = 1; }
    if (rd4(img + 52) != 1) { emit("[cc500run] p_type != PT_LOAD\n"); bad = 1; }
    if (rd4(img + 68) != len) { emit("[cc500run] p_filesz!=size\n"); bad = 1; }
    if ((unsigned)rd4(img + 60) != BASE) { emit("[cc500run] p_vaddr bad\n"); bad = 1; }
    /* 入口 stub：0x54 mov eax,[esp+8]…，0x5E e8，0x63 89 c3 31 c0 cd 80 */
    if (!(img[0x54] == 0x8b && img[0x55] == 0x44 && img[0x56] == 0x24 && img[0x5e] == 0xe8 &&
          img[0x63] == 0x89 && img[0x64] == 0xc3 && img[0x65] == 0x31 &&
          img[0x66] == 0xc0 && img[0x67] == 0xcd && img[0x68] == 0x80)) {
        emit("[cc500run] entry stub mismatch at 0x54/0x63\n"); bad = 1;
    }
    if (check_only) { emit_out(bad ? "[check] BAD\n" : "[check] OK\n"); return bad ? 1 : 0; }
    if (bad) {
        img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
        img[4] = 1; img[5] = 1; img[6] = 1;
        emit("[cc500run] header repaired locally (mini-os elf_load would reject)\n");
    }

    long m = mmap2x(BASE, (len + 4095) & ~4095, 7, 0x32, -1, 0);
    if (m != (long)BASE) die("mmap fail ", (int)m);
    unsigned char *lo = (unsigned char *)BASE;
    for (int i = 0; i < len; i++) lo[i] = img[i];

    /* 1) 退出桥 */
    lo[0x63] = 0xe9;
    wr4(lo + 0x64, (int)((uintptr_t)&exit_bridge - (BASE + 0x63 + 5)));
    lo[0x68] = 0x90;   /* 残留字节（0x63..0x68 六字节内的最后一个） */
    /* 2) syscall3 stub → cc_bridge */
    static const unsigned char pat[18] = {
        0x8b,0x44,0x24,0x10, 0x8b,0x5c,0x24,0x0c, 0x8b,0x4c,0x24,0x08,
        0x8b,0x54,0x24,0x04, 0xcd,0x80 };
    int nsite = 0;
    for (int i = 0x69; i + 19 <= len; i++) {
        int hit = 1;
        for (int j = 0; j < 18; j++) if (lo[i + j] != pat[j]) { hit = 0; break; }
        if (!hit) continue;
        lo[i] = 0xe9;
        wr4(lo + i + 1, (int)((uintptr_t)&cc_bridge - (BASE + i + 5)));
        lo[i + 5] = 0x90;   /* 盖掉 stub 第 6 字节；其余 stub 字节被跳过 */
        nsite++;
        i += 18;
    }
    if (nsite == 0) { /* 纯计算产物无 syscall3 stub？cc500 恒 emit → 不应发生 */ }

    /* 初始栈：字符串 + argv + [esp]=0 [esp+4]=argc [esp+8]=argv */
    install_segv();
    long st = mmap2x(0x60000000u, 8u << 20, 3, 0x32, -1, 0);
    if (st != 0x60000000) die("stack mmap fail ", (int)st);
    unsigned char *sp = (unsigned char *)0x60000000u; /* 区域基址（索引一律用 off-base） */
    unsigned top = 0x60800000u;
    unsigned strs = top - 4096;
    unsigned argvp = strs - 64;
    unsigned esp0 = argvp - 32;
    /* 写 argv 字符串 */
    unsigned off = strs;
    unsigned vals[16]; int np = 0;
    vals[np++] = (unsigned)argv[i0];                    /* 产物 argv[0] = 产物路径（内核约定） */
    for (int i = i0 + 1; i < argc && np < 15; i++) vals[np++] = (unsigned)argv[i];
    unsigned addrs[16];
    for (int i = 0; i < np; i++) {
        const char *src = (const char *)(uintptr_t)vals[i];
        addrs[i] = off;
        int k = 0;
        while (src[k]) { sp[(off - 0x60000000u) + k] = src[k]; k++; }
        sp[(off - 0x60000000u) + k] = 0;
        off = off + k + 1;
    }
    for (int i = 0; i < np; i++) wr4(sp + (argvp - 0x60000000u) + 4 * i, (int)addrs[i]);
    wr4(sp + (esp0 - 0x60000000u) + 0, 0);
    wr4(sp + (esp0 - 0x60000000u) + 4, np);
    wr4(sp + (esp0 - 0x60000000u) + 8, (int)argvp);

    if (getenv_dbg) {
        emit("[dbg] stack dump esp0-64 .. esp0+32:\n");
        for (int q = -16; q < 8; q++) {
            emit("  "); decout(q * 4); emit(": ");
            hexout(rd4(sp + (esp0 - 0x60000000u) + q * 4)); emit("\n");
        }
        emit("[dbg] argv array @argvp: ");
        for (int q = 0; q < np; q++) { hexout(rd4(sp + (argvp - 0x60000000u) + 4 * q)); emit(" "); }
        emit("\n[dbg] str@addrs[0]: ");
        emit_out((char *)(uintptr_t)addrs[0]);
        emit("\n");
    }
    enter_prod((void *)(BASE + 0x54), (void *)esp0);
    die("product returned?", 6);
    return 9;
}
