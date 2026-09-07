/* harness/crt32.c — hostminicc 的 Linux-i386 freestanding shim（评委环境无 gcc-multilib/qemu-i386）。
 * 语义对齐仓库 tools/minicc/host_crt.c 的 mini-os syscall 契约：
 *   0=exit 1=print 13=create 14=open(slot) 15=write(slot) 16=read(slot) 17=close 19=unlink 35=brk
 * 只承载"评估宿主"，不改动被评对象 minicc.c 一行。 */
#include <stdint.h>

int minicc_main(char *argv, int argc);

static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}
#define NR_exit   1
#define NR_read   3
#define NR_write  4
#define NR_open   5
#define NR_close  6
#define NR_unlink 10

static int fdtab[8];

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
#define O_CREAT 0x40
#define O_TRUNC 0x200

int syscall3(int n, int a, int b, int c) {
    int fd, slot, r, w; long rr;
    switch (n) {
    case 0:  sys3(NR_exit, a & 0xff, 0, 0); for (;;) {}
    case 1:  return (int)sys3(NR_write, 1, (long)(uintptr_t)a, my_strlen((const char *)(uintptr_t)a));
    case 13: { int x; (void)x;
        fd = (int)sys3(NR_open, (long)(uintptr_t)a, 2 | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return -1;
        sys3(NR_close, fd, 0, 0);
        return 0;
    }
    case 14: {
        if (a <= 0 || a >= 8 || fdtab[a]) return -1;
        fd = (int)sys3(NR_open, (long)(uintptr_t)b, c ? (2 | O_CREAT) : 0, 0644);
        if (fd < 0) return -1;
        fdtab[a] = fd;
        return 0;
    }
    case 15:
        if (a <= 0 || a >= 8 || !fdtab[a]) return -1;
        return (int)sys3(NR_write, fdtab[a], (long)(uintptr_t)b, c);
    case 16:
        if (a <= 0 || a >= 8 || !fdtab[a]) return -1;
        return (int)sys3(NR_read, fdtab[a], (long)(uintptr_t)b, c);
    case 17:
        if (a > 0 && a < 8 && fdtab[a]) { sys3(NR_close, fdtab[a], 0, 0); fdtab[a] = 0; }
        return 0;
    case 19: return sys3(NR_unlink, (long)(uintptr_t)a, 0, 0) == 0 ? 0 : -1;
    case 35: {                                   /* SYS_BRK：直接用 Linux i386 brk，语义最贴近 */
        if (a == 0) return (int)sys3(45, 0, 0, 0);
        rr = sys3(45, (uintptr_t)a, 0, 0);
        return (rr == (long)(uintptr_t)a) ? 0 : -1;
    }
    default: return -1;
    }
    return -1;
}

void *memcpy(void *d, const void *s, int n) {
    unsigned char *p = d; const unsigned char *q = s; int i;
    for (i = 0; i < n; i++) p[i] = q[i];
    return d;
}
void *memset(void *d, int v, int n) {
    unsigned char *p = d; int i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)v;
    return d;
}

int crt_main(int argc, char **argv) {
    int rc = minicc_main((char *)argv, argc);
    sys3(NR_exit, rc & 0xff, 0, 0);
    for (;;) {}
    return rc;
}
