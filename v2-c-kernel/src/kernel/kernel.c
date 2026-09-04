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
#include "storage.h"
#include "ata.h"
#include "e1000.h"
#include "netif.h"     /* v1.1 Step 1：netif 抽象层——协议/系统代码不再直调具体网卡 */
#include "uart_netif.h" /* v1.1 Step 2：COM2 串口网卡（SLIP）——第二个 netif 后端 */
#include "userprog_offsets.h"
#include "version.h"   /* v0.30（评估 L-4）：版本单一来源，启动横幅取 MINI_OS_VERSION */
#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

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

    /* 加固 A-1 ①：尽早随机化内核栈金丝雀（防固定值被绕过）。
     * 须在 serial_init 之后（ssp_seed 印日志）；此时内核各子系统函数尚未大量入栈，
     * 之后的函数序言/返回前读的都是新值，语义一致。 */
    extern void ssp_seed(void);
    ssp_seed();

    vga_puts("Mini-OS " MINI_OS_VERSION "  (toolchain self-host: cc500 compiles itself in guest)\n");
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

    ata_init();       /* v0.16：探测 IDE 真盘（无盘则纯内存盘） */
    storage_init();   /* v0.16：ramdisk + 真盘加载/格式化 + initramfs */
    /* ---- v1.1 netif 网卡注册（Step 1 e1000 / Step 2 串口 SLIP）----
     * 默认 e1000 优先（先注册先选用）；测试才用 UART_NETIF_DEFAULT 让串口优先
     * （静态绑定，D6——不实现路由表）。netif_init_all 内触发各自驱动 init。 */
#ifdef UART_NETIF_DEFAULT
    uart_netif_register();
    e1000_netif_register();
#else
    e1000_netif_register();
    uart_netif_register();
#endif
    netif_init_all(); /* 初始化并选定当前网卡（无网卡则跳过，netif_ready()=-1） */
    e1000_dhcp_run(); /* v0.25：DISCOVER->OFFER->REQUEST->ACK 动态取 IP/网关（失败回退静态） */
    e1000_selftest(); /* v0.18：ARP 请求/应答自检（验证 TX+RX） */
    e1000_udp_selftest(); /* v0.19：经 SLIRP 网关回环到宿主 UDP echo（PING/PONG） */
    e1000_icmp_selftest(); /* v0.23：ICMP Echo 自检——PING 通宿主（SLIRP 网关回显） */

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

    /* v0.20：网络可用时启动用户态 UDP socket 演示（sockdemo 用 sys_net_* 系统调用
     * 与宿主 UDP echo 服务端到端回环；依赖 ARP 自检已学到网关 MAC）。
     * 无网卡（e1000_ready()=-1）则不生成，避免无网络环境下的噪音。 */
    if (netif_ready() == 0) {
        int sock_pid = usermode_spawn_elf("sockdemo", APP_LINK, 0);
        serial_printf("[boot] sockdemo pid=%d\n", sock_pid);
    }
#ifdef TCP_DEMO
    /* v1.1 Step 4：TCP_DEMO 构建时自动 spawn 虚拟 TCP HTTP demo（宿主转发器 + HTTP 服务
     * 由 tests/test_tcp.sh 提供）。不依赖网卡类型：e1000 走 SLIRP/UDP，串口走 SLIP/IP。 */
    if (netif_ready() == 0) {
        int tcp_pid = usermode_spawn_elf("httpdemo", APP_LINK, 0);
        serial_printf("[boot] httpdemo pid=%d\n", tcp_pid);
    }
#endif
#ifdef DL_DEMO
    /* v1.2：DL_DEMO 构建时自动 spawn 大文件下载 demo（宿主 128KB 文件服务由
     * tests/test_tcp_dl.sh 提供）。只验证大文件下载路径，与 httpdemo 互不占用连接表。 */
    if (netif_ready() == 0) {
        int dl_pid = usermode_spawn_elf("dldemo", APP_LINK, 0);
        serial_printf("[boot] dldemo pid=%d\n", dl_pid);
    }
#endif

    /* 切入第一个就绪进程（不返回）；中断由 iret 的 eflags 开启 */
    sched_start();
    for (;;);   /* 不应到达 */
}
