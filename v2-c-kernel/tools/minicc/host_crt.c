/* mini-os/v2-c-kernel/tools/minicc/host_crt.c
 * 宿主 shim：把 minicc.c 编成 Linux 宿主二进制（模拟 mini-os int $0x80 契约）。
 * 与 cc500 的 host_crt.c 同思路；minicc.c 只依赖下述 syscall3 编号：
 *   0=exit 1=print 13=create 14=open(slot) 15=write(slot) 16=read(slot)
 *   17=close(slot) 19=delete 35=brk
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int minicc_main(char *argv, int argc);

static int fdtab[8];
static off_t fdpos[8];

static char arena_mem[256u << 20];
static uint32_t base_brk = 0, cur_brk = 0;

int syscall3(int n, int a, int b, int c) {
    switch (n) {
    case 0:  _exit(a & 0xff); break;                    /* SYS_EXIT */
    case 1:  return (int)write(1, (const void *)(uintptr_t)a,
                               (size_t)strlen((const char *)(uintptr_t)a));
    case 13: {                                         /* SYS_FS_CREATE（清空文件） */
        int fd = open((const char *)(uintptr_t)a, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return -1;
        close(fd);
        return 0;
    }
    case 14: {                                         /* SYS_FS_OPEN slot */
        int slot = a;
        if (slot <= 0 || slot >= 8 || fdtab[slot]) return -1;
        int fd = open((const char *)(uintptr_t)b, c ? O_RDWR | O_CREAT : O_RDONLY, 0644);
        if (fd < 0) return -1;
        fdtab[slot] = fd; fdpos[slot] = 0;
        return 0;
    }
    case 15: {                                         /* SYS_FS_WRITE slot */
        int w = (int)pwrite(fdtab[a], (const void *)(uintptr_t)b, (size_t)c, fdpos[a]);
        if (w > 0) fdpos[a] += w;
        return w;
    }
    case 16: {                                         /* SYS_FS_READ slot */
        int r = (int)pread(fdtab[a], (void *)(uintptr_t)b, (size_t)c, fdpos[a]);
        if (r > 0) fdpos[a] += r;
        return r;
    }
    case 17: close(fdtab[a]); fdtab[a] = 0; return 0;  /* SYS_FS_CLOSE slot */
    case 19: unlink((const char *)(uintptr_t)a); return 0;
    case 35: {                                         /* SYS_BRK（静态竞技场） */
        if (!cur_brk) { base_brk = (uint32_t)(uintptr_t)arena_mem; cur_brk = base_brk; }
        if (a == 0) return (int)cur_brk;
        if ((uint32_t)a >= base_brk && (uint32_t)a < base_brk + (256u << 20)) {
            cur_brk = (uint32_t)a;
            return 0;
        }
        return -1;
    }
    default: return -1;
    }
    return -1;
}

int main(int argc, char **argv) {
    int rc = minicc_main((char *)argv, argc);
    fprintf(stderr, "[hostminicc] rc=%d\n", rc);
    return rc;
}
