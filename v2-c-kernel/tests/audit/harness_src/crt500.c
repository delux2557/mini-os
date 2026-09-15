/* crt500.c — cc500 宿主 freestanding shim（同 crt32 语义；入口 cc500_main(argv, argc)） */
#include <stdint.h>
int cc500_main(char *argv, int argc);
static long sys3(long n, long a, long b, long c) {
    long r; __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r;
}
static int fdtab[8];
static int slen(const char *s){int n=0;while(s[n])n++;return n;}
int syscall3(int n, int a, int b, int c) {
    int fd;
    switch (n) {
    case 0:  sys3(1, a & 0xff, 0, 0); for(;;){}
    case 1:  return (int)sys3(4, 1, (long)(uintptr_t)a, slen((const char *)(uintptr_t)a));
    case 13: fd = (int)sys3(5, (long)(uintptr_t)a, 2|0x40|0x200, 0644);
             if (fd < 0) return -1; sys3(6, fd, 0, 0); return 0;
    case 14: if (a<=0||a>=8||fdtab[a]) return -1;
             fd = (int)sys3(5, (long)(uintptr_t)b, c? (2|0x40):0, 0644);
             if (fd<0) return -1; fdtab[a]=fd; return 0;
    case 15: if (a<=0||a>=8||!fdtab[a]) return -1; return (int)sys3(4, fdtab[a], (long)(uintptr_t)b, c);
    case 16: if (a<=0||a>=8||!fdtab[a]) return -1; return (int)sys3(3, fdtab[a], (long)(uintptr_t)b, c);
    case 17: if (a>0&&a<8&&fdtab[a]){ sys3(6, fdtab[a],0,0); fdtab[a]=0; } return 0;
    case 19: return sys3(10, (long)(uintptr_t)a, 0, 0)==0?0:-1;
    case 35: if (a==0) return (int)sys3(45,0,0,0);
             { long r = sys3(45,(uintptr_t)a,0,0); return r==(long)(uintptr_t)a?0:-1; }
    default: return -1;
    }
}
int crt_main(int argc, char **argv) {
    int rc = cc500_main((char *)argv, argc);
    sys3(1, rc & 0xff, 0, 0);
    for(;;){}
    return rc;
}
