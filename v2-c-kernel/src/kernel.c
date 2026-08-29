/* mini-os/v2-c-kernel/kernel.c
 * 主内核（v0.5）：初始化各子系统 -> 内存自检 -> 创建多进程 -> 调度器接管
 * 由 boot.s 以 multiboot 协议加载，cdecl 传入 (magic, multiboot_info) */
#include "vga.h"
#include "serial.h"
#include "idt.h"
#include "timer.h"
#include "kb.h"
#include "mem.h"
#include "heap.h"
#include "usermode.h"
#include "sched.h"
#include "blockdev.h"
#include "fs.h"
#include "userprog_offsets.h"
#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* ---- v0.8 文件系统：内存盘（ramdisk）后端 ---- */
#define RAMDISK_BLOCKS 256   /* 1MB：数据块 252 个，足够演示多文件/跨块写 */
static blockdev_t ramdisk;

static void ramdisk_init(void) {
    uint32_t phys = frame_alloc_run(RAMDISK_BLOCKS);   /* 申请连续物理帧 */
    if (!phys) {
        vga_puts("[fs] FATAL: ramdisk alloc failed\n");
        serial_puts("[fs] FATAL: ramdisk alloc failed\n");
        for (;;);
    }
    blockdev_init(&ramdisk, (uint8_t *)phys, RAMDISK_BLOCKS);
    fs_mount(&ramdisk);
    int rc = fs_init(&ramdisk);                        /* 格式化并建根目录 */
    vga_printf("[fs] ramdisk %u blocks @%x (1MB), format %s\n",
               RAMDISK_BLOCKS, phys, rc == 0 ? "ok" : "FAIL");
    serial_printf("[fs] ramdisk %u blocks @%x (1MB), format %s\n",
                  RAMDISK_BLOCKS, phys, rc == 0 ? "ok" : "FAIL");
}

/* ---- v0.9 initramfs：把嵌入式文件（motd + 各应用的 ELF 文件）写入 ramdisk ----
 * 应用由 Makefile 编译链接到固定地址后以 ld -r -b binary 整体内嵌进内核，
 * 启动时作为文件落盘；shell 由内核直接加载常驻，其余由 shell 的 run 命令加载。 */
extern char _binary_hello_elf_start[], _binary_hello_elf_end[];
extern char _binary_echo_elf_start[],  _binary_echo_elf_end[];
extern char _binary_crash_elf_start[], _binary_crash_elf_end[];
extern char _binary_isol_elf_start[],  _binary_isol_elf_end[];
extern char _binary_forkdemo_elf_start[], _binary_forkdemo_elf_end[];
extern char _binary_args_elf_start[],   _binary_args_elf_end[];
extern char _binary_stackovf_elf_start[], _binary_stackovf_elf_end[];
extern char _binary_shell_elf_start[], _binary_shell_elf_end[];

static void initramfs_file(const char *name, const void *data, uint32_t len) {
    int ino = fs_create(fs_device(), name);
    if (ino < 0) {
        serial_printf("[ramdisk] create '%s' failed\n", name);
        return;
    }
    int n = fs_write(fs_device(), (uint32_t)ino, data, 0, len);
    serial_printf("[ramdisk] '%s' %u bytes inode=%d write=%d\n", name, len, ino, n);
}

static void initramfs_setup(void) {
    static const char motd[] =
        "Mini-OS v0.13: user stack guard pages + overflow detection.\n"
        "Commands: help ls cat run exec exit   (try: run stackovf)\n";
    initramfs_file("motd", motd, (uint32_t)(sizeof(motd) - 1));
    initramfs_file("hello",
                   _binary_hello_elf_start,
                   (uint32_t)(_binary_hello_elf_end - _binary_hello_elf_start));
    initramfs_file("echo",
                   _binary_echo_elf_start,
                   (uint32_t)(_binary_echo_elf_end - _binary_echo_elf_start));
    initramfs_file("crash",
                   _binary_crash_elf_start,
                   (uint32_t)(_binary_crash_elf_end - _binary_crash_elf_start));
    initramfs_file("isol",
                   _binary_isol_elf_start,
                   (uint32_t)(_binary_isol_elf_end - _binary_isol_elf_start));
    initramfs_file("forkdemo",
                   _binary_forkdemo_elf_start,
                   (uint32_t)(_binary_forkdemo_elf_end - _binary_forkdemo_elf_start));
    initramfs_file("args",
                   _binary_args_elf_start,
                   (uint32_t)(_binary_args_elf_end - _binary_args_elf_start));
    initramfs_file("stackovf",
                   _binary_stackovf_elf_start,
                   (uint32_t)(_binary_stackovf_elf_end - _binary_stackovf_elf_start));
    initramfs_file("shell",
                   _binary_shell_elf_start,
                   (uint32_t)(_binary_shell_elf_end - _binary_shell_elf_start));
}

extern uint32_t _kernel_start, _kernel_end;

/* 内存管理自检（精简为一行汇总） */
static void memory_selftest(void) {
    /* 帧分配器 */
    uint32_t f1 = frame_alloc();
    uint32_t f2 = frame_alloc_run(3);
    frame_free(f1);
    frame_free(f2);
    frame_free(f2 + 4096);
    frame_free(f2 + 8192);

    /* 内核堆 */
    int *a = (int *)kmalloc(100);
    int *b = (int *)kmalloc(64);
    int *c = (int *)kmalloc(32);
    if (a && b && c) {
        for (int i = 0; i < 25; i++) a[i] = i;
        for (int i = 0; i < 16; i++) b[i] = 1000 + i;
        c[0] = 0xC0FFEE;
        kfree(b);
        kfree(a);
        kfree(c);
    }

    /* 懒分配 */
    volatile uint32_t *lp = (volatile uint32_t *)0x40001000;
    *lp = 42;

    vga_printf("[memtest] frames+heap+lazy OK (free %u KB, lazy %u)\n",
               free_memory_kb(), lazy_page_count());
    serial_printf("[memtest] frames+heap+lazy OK (free %u KB, lazy %u)\n",
                  free_memory_kb(), lazy_page_count());
}

void kernel_main(uint32_t magic, uint32_t mb_info) {
    vga_init();
    serial_init();

    vga_puts("Micro-OS v0.13  (user stack guard pages + overflow detection)\n");
    serial_puts("[boot] VGA + serial ready\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        vga_puts("[!] multiboot magic mismatch\n");
        serial_puts("[!] multiboot magic mismatch\n");
    }

    idt_init();           /* 载入 IDT：异常/页错误/0x80 系统调用门 */
    mem_init(mb_info);    /* 检测内存 + 保留内核区 */
    paging_init();        /* 开启分页 */
    heap_init();          /* 内核堆 */
    usermode_init();      /* 重建 GDT(含 ring3 段) + 加载 TSS */

    vga_printf("[mem] total=%u KB, kernel=[%x,%x), free=%u KB\n",
               total_memory_kb(), (uint32_t)&_kernel_start, (uint32_t)&_kernel_end,
               free_memory_kb());
    serial_printf("[mem] total=%u KB, kernel=[%x,%x), free=%u KB\n",
                  total_memory_kb(), (uint32_t)&_kernel_start, (uint32_t)&_kernel_end,
                  free_memory_kb());

    memory_selftest();

    ramdisk_init();   /* v0.8：内存盘 + 格式化文件系统 */
    initramfs_setup();  /* v0.9：写入 motd + 各应用 ELF blob */

    timer_init(100);      /* 100 Hz 心跳 */
    kb_init();
    /* v0.10：串口 COM1 接收 -> 键盘行缓冲，`qemu -serial stdio` 即成可交互串口终端 */
    serial_set_rx_hook(kb_feed_char);

    /* ---- v0.5：多进程 + 抢占调度；v0.6：信号量/共享内存 IPC 演示 ---- */
    vga_puts("[ok] subsystems ready; creating processes...\n");
    serial_puts("[ok] subsystems ready; creating processes\n");

    sched_init();
    sched_spawn(USER_MAIN_A_OFF, "procA");
    sched_spawn(USER_MAIN_B_OFF, "procB");
    sched_spawn(USER_SEM_A_OFF,  "procSemA");
    sched_spawn(USER_SEM_B_OFF,  "procSemB");
    sched_spawn(USER_CRASH_OFF,  "procCrash");
    /* v0.7：消息队列演示（消费者先建，立即在空缓冲上阻塞，演示 recv-block） */
    sched_spawn(USER_MSG_C_OFF,  "procMsgC");
    sched_spawn(USER_MSG_P_OFF,  "procMsgP");
    /* v0.8：文件系统演示（写入-读回校验 + ls 列目录） */
    sched_spawn(USER_FS_W_OFF,   "procFSA");
    sched_spawn(USER_FS_L_OFF,   "procFSB");

    /* v0.9：从文件系统加载常驻 shell（交互式入口）。
     * 键盘行完成回调 -> sched_wake_keyboard：唤醒阻塞在 sys_readline 上的进程。 */
    kb_set_line_hook(sched_wake_keyboard);
    int shell_pid = usermode_spawn_elf("shell", SHELL_LINK, 1);
    serial_printf("[boot] shell pid=%d\n", shell_pid);

    /* 切入第一个就绪进程（不返回）；中断由 iret 的 eflags 开启 */
    sched_start();
    for (;;);   /* 不应到达 */
}
