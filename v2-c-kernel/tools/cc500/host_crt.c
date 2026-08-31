/* 宿主 shim：把 cc500 编成 Linux 宿主二进制（模拟 mini-os int 0x80 契约） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
int cc500_main(char *argv, int argc);
static int fdtab[8]; static off_t fdpos[8];
static char arena_mem[256u<<20]; static char *arena=0; static uint32_t cur_brk, base_brk;
int syscall3(int n, int a, int b, int c) {
    switch (n) {
    case 0: _exit(a & 0xff);
    case 1: { char *s = (char*)(uint32_t)a; size_t l = 0; while (((uint8_t*)s)[l]) l++;
              return (int)write(1, s, l); }
    case 13: { int fd = open((char*)(uint32_t)a, O_RDWR|O_CREAT|O_TRUNC, 0644); return fd; }
    case 19: unlink((char*)(uint32_t)a); return 0;
    case 14: { int slot = a; char *p = (char*)(uint32_t)b; int mode = c;
              { size_t l=0; while (((uint8_t*)p)[l] && l<255) l++; char buf[300]; memcpy(buf,p,l); buf[l]=0;
                if (slot==0 || slot>=8 || fdtab[slot]) return -1;
                int fd = open(buf, mode? O_RDWR|O_CREAT : O_RDONLY, 0644);
                if (fd<0) return -1; fdtab[slot]=fd; return 0; } }
    case 15: { int slot=a; int w=(int)pwrite(fdtab[slot], (void*)(uint32_t)b, (size_t)c, fdpos[slot]); if(w>0) fdpos[slot]+=w; return w; }
    case 16: { int slot=a; int r=(int)pread(fdtab[slot], (void*)(uint32_t)b, (size_t)c, fdpos[slot]); if(r>0) fdpos[slot]+=r; return r; }
    case 17: { close(fdtab[a]); fdtab[a]=0; return 0; }
    case 35: { if (!arena){arena=arena_mem; base_brk=cur_brk=(uint32_t)(uintptr_t)arena;}
              if (a==0) return (int)cur_brk;
              if (a>=base_brk && a<base_brk+(256u<<20)){ cur_brk=(uint32_t)a; return 0; }
              return -1; }
    default: return -1;
    }
}
int main(int argc, char **argv) {
    int rc = cc500_main((char*)argv, argc);
    fprintf(stderr, "[hostcc] cc500_main rc=%d\n", rc); _exit(rc&0xff);
}