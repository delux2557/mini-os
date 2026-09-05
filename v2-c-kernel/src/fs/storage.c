/* mini-os/v2-c-kernel/src/storage.c
 * 存储子系统（v0.16）：ramdisk 块设备 + ATA 真盘持久化。
 *  - 无盘（无 -hda）：纯内存盘，格式化 + initramfs（v0.8/v0.9 原行为，重启丢失）
 *  - 有盘：整盘读入 ramdisk；若盘上有有效 FS（超级块 magic）则直接挂载
 *          （跳过格式化/initramfs，磁盘即真源，用户数据跨重启存活）；
 *          否则格式化 + 写 initramfs，并落盘一次使磁盘具备有效镜像。
 *  - storage_sync()：把 ramdisk 全量写回磁盘（sys_fs_sync / shell save 命令）。
 */
#include "storage.h"
#include "blockdev.h"
#include "fs.h"
#include "ata.h"
#include "mem.h"
#include "serial.h"
#include "vga.h"
#include "version.h"   /* v0.30（评估 L-4）：motd 版本串单一来源 */
#include <stdint.h>

#define RAMDISK_BLOCKS 768           /* 3MB：initramfs（含 V3 自举 minicc-self 686KB + minicc.c 40KB）
                                      * + 运行时 /out.elf(686KB) 峰值 ~1.8MB；256(1MB) 会写穿失败（shell 缺） */
#define SECTORS_PER_BLOCK (BLOCK_SIZE / ATA_SECTOR_SIZE)   /* 4096/512 = 8 */

static blockdev_t ramdisk;
static int persistent = 0;            /* 磁盘上是否有有效 FS（重启后仍可挂载） */

static uint8_t sector_buf[ATA_SECTOR_SIZE];   /* 单扇区暂存（避免内核栈大数组） */

/* 从 ATA 整盘读入 ramdisk（扇区 -> 块内偏移） */
static void disk_load(void) {
    uint32_t nsectors = ata_sectors();
    if (nsectors > RAMDISK_BLOCKS * SECTORS_PER_BLOCK)
        nsectors = RAMDISK_BLOCKS * SECTORS_PER_BLOCK;
    for (uint32_t s = 0; s < nsectors; s++) {
        if (ata_read_sectors(s, 1, sector_buf) < 0) break;
        blockdev_write(&ramdisk, s / SECTORS_PER_BLOCK,
                       (s % SECTORS_PER_BLOCK) * ATA_SECTOR_SIZE,
                       sector_buf, ATA_SECTOR_SIZE);
    }
    serial_printf("[storage] loaded %u sectors from disk\n", nsectors);
    vga_printf("[storage] loaded %u sectors from disk\n", nsectors);
}

/* 把 ramdisk 全量写回 ATA */
static void disk_save(void) {
    uint32_t nsectors = ata_sectors();
    if (nsectors > RAMDISK_BLOCKS * SECTORS_PER_BLOCK)
        nsectors = RAMDISK_BLOCKS * SECTORS_PER_BLOCK;
    uint32_t ok = 0;
    for (uint32_t s = 0; s < nsectors; s++) {
        blockdev_read(&ramdisk, s / SECTORS_PER_BLOCK,
                      (s % SECTORS_PER_BLOCK) * ATA_SECTOR_SIZE,
                      sector_buf, ATA_SECTOR_SIZE);
        if (ata_write_sectors(s, 1, sector_buf) == 0) ok++;
    }
    serial_printf("[storage] saved %u/%u sectors to disk\n", ok, nsectors);
    vga_printf("[storage] saved %u/%u sectors to disk\n", ok, nsectors);
}

int storage_sync(void) {
    if (!persistent) return -1;       /* 无盘：无持久化后端 */
    disk_save();
    return 0;
}

/* ---- v0.9 initramfs：把嵌入式文件（motd + 各应用的 ELF 文件）写入 FS ---- */
extern char _binary_hello_elf_start[], _binary_hello_elf_end[];
extern char _binary_echo_elf_start[],  _binary_echo_elf_end[];
extern char _binary_crash_elf_start[], _binary_crash_elf_end[];
extern char _binary_isol_elf_start[],  _binary_isol_elf_end[];
extern char _binary_forkdemo_elf_start[], _binary_forkdemo_elf_end[];
extern char _binary_args_elf_start[],   _binary_args_elf_end[];
extern char _binary_stackovf_elf_start[], _binary_stackovf_elf_end[];
extern char _binary_deep_elf_start[], _binary_deep_elf_end[];
extern char _binary_deepfork_elf_start[], _binary_deepfork_elf_end[];   /* v0.29 已生长栈×fork */
extern char _binary_deepexec_elf_start[], _binary_deepexec_elf_end[];   /* v0.29 已生长栈×exec */
extern char _binary_heapdemo_elf_start[], _binary_heapdemo_elf_end[];
extern char _binary_fsdemo_elf_start[],  _binary_fsdemo_elf_end[];
extern char _binary_waitdemo_elf_start[], _binary_waitdemo_elf_end[];
extern char _binary_abuse_elf_start[],   _binary_abuse_elf_end[];
extern char _binary_sockdemo_elf_start[], _binary_sockdemo_elf_end[];
extern char _binary_bigdemo_elf_start[], _binary_bigdemo_elf_end[];
extern char _binary_cc500_elf_start[],  _binary_cc500_elf_end[];     /* v0.27 编译器 ELF */
extern char _binary_cc500_c_start[],    _binary_cc500_c_end[];       /* v0.27 编译器源码 */
extern char _binary_minicc_elf_start[], _binary_minicc_elf_end[];    /* V1 自研编译器 ELF */
extern char _binary_minicc_self_elf_start[], _binary_minicc_self_elf_end[]; /* V3 自举 P1 ELF */
extern char _binary_minicc_self_c_start[], _binary_minicc_self_c_end[];     /* V3 自举源码（/minicc.c） */
extern char _binary_shell_elf_start[], _binary_shell_elf_end[];
extern char _binary_httpdemo_elf_start[], _binary_httpdemo_elf_end[];  /* v1.1 Step 4 虚拟 TCP */
extern char _binary_dldemo_elf_start[],    _binary_dldemo_elf_end[];    /* v1.2 大文件下载 */
extern char _binary_cansmash_elf_start[], _binary_cansmash_elf_end[];   /* v1.5 P2 栈金丝雀 */
extern char _binary_sandboxdemo_elf_start[], _binary_sandboxdemo_elf_end[]; /* v0.34 BUG-058 */
extern char _binary_badinsn_elf_start[], _binary_badinsn_elf_end[];   /* SEC-01 回归: ring3 ud2 (#UD) */
extern char _binary_chaos_elf_start[], _binary_chaos_elf_end[];       /* 加固 A-1 ④: ring3 随机坏指令探针 */

/* ---- V4 mini-Csmith guest 差分（方案 B，仅 GUEST_DIFF=1 时启用） ----
 * 差分对拍把 minicc 编译的随机样本 .elf 嵌入 initramfs，guest 内 `run dsXX` 拿语义退码
 * 与 gcc 参考差分。默认构建不 -DGUEST_DIFF ⇒ 零侵入（这些符号/文件不存在也不引用）。
 * 样本 .elf 由 tools/minicc/diffsynth/gen_guest_elfs.sh 构建期生成（ds00..ds11）。 */
#ifdef GUEST_DIFF
extern char _binary_ds00_elf_start[], _binary_ds00_elf_end[];
extern char _binary_ds01_elf_start[], _binary_ds01_elf_end[];
extern char _binary_ds02_elf_start[], _binary_ds02_elf_end[];
extern char _binary_ds03_elf_start[], _binary_ds03_elf_end[];
extern char _binary_ds04_elf_start[], _binary_ds04_elf_end[];
extern char _binary_ds05_elf_start[], _binary_ds05_elf_end[];
extern char _binary_ds06_elf_start[], _binary_ds06_elf_end[];
extern char _binary_ds07_elf_start[], _binary_ds07_elf_end[];
extern char _binary_ds08_elf_start[], _binary_ds08_elf_end[];
extern char _binary_ds09_elf_start[], _binary_ds09_elf_end[];
extern char _binary_ds10_elf_start[], _binary_ds10_elf_end[];
extern char _binary_ds11_elf_start[], _binary_ds11_elf_end[];
#endif

static void initramfs_file(const char *name, const void *data, uint32_t len) {
    int ino = fs_create(fs_device(), name);
    if (ino < 0) {
        serial_printf("[ramdisk] create '%s' failed\n", name);
        return;
    }
    int n = fs_write(fs_device(), (uint32_t)ino, data, 0, len);
    /* BUG-057：initramfs 系统文件只读——先 write 后 protect（避免自锁）；退出前已落盘。
     * 无盘（每次重建）与有盘首启格式化共用本函数，两级权限自动覆盖两条路径。 */
    if (n > 0) fs_protect(fs_device(), name);
    serial_printf("[ramdisk] '%s' %u bytes inode=%d write=%d\n", name, len, ino, n);
}

static void initramfs_setup(void) {
    static const char motd[] =
        "Mini-OS " MINI_OS_VERSION ": toolchain self-host (cc500 compiles itself). Try: ccboot\n"
        "Commands: help ls cat mkdir rmdir rm run exec save selftest exit netping ccboot\n";
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
    initramfs_file("deep",
                   _binary_deep_elf_start,
                   (uint32_t)(_binary_deep_elf_end - _binary_deep_elf_start));
    /* v0.29 回归盲区：已生长栈 × fork / exec 组合演示 */
    initramfs_file("deepfork",
                   _binary_deepfork_elf_start,
                   (uint32_t)(_binary_deepfork_elf_end - _binary_deepfork_elf_start));
    initramfs_file("deepexec",
                   _binary_deepexec_elf_start,
                   (uint32_t)(_binary_deepexec_elf_end - _binary_deepexec_elf_start));
    initramfs_file("heapdemo",
                   _binary_heapdemo_elf_start,
                   (uint32_t)(_binary_heapdemo_elf_end - _binary_heapdemo_elf_start));
    initramfs_file("fsdemo",
                   _binary_fsdemo_elf_start,
                   (uint32_t)(_binary_fsdemo_elf_end - _binary_fsdemo_elf_start));
    initramfs_file("waitdemo",
                   _binary_waitdemo_elf_start,
                   (uint32_t)(_binary_waitdemo_elf_end - _binary_waitdemo_elf_start));
    initramfs_file("abuse",
                   _binary_abuse_elf_start,
                   (uint32_t)(_binary_abuse_elf_end - _binary_abuse_elf_start));
    initramfs_file("sockdemo",
                   _binary_sockdemo_elf_start,
                   (uint32_t)(_binary_sockdemo_elf_end - _binary_sockdemo_elf_start));
    /* DoS 回归夹具 `zbig`：84B 畸形 ELF，p_memsz=0x06001000(96MB)、p_filesz=0。
     * 若加载器未在 load_elf_file 钳制区间（BUG-056），run zbig 会触发巨大映射；门禁
     * 断言其必被 -1 拒绝且整机不 [FATAL]（qemu_regression/test_serial 覆盖）。 */
    {
        static const unsigned char zbig[] = { 0x7f,0x45,0x4c,0x46,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x03,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x34,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x34,0x00,0x20,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x06,0x07,0x00,0x00,0x00,0x00,0x10,0x00,0x00 };
        initramfs_file("zbig", zbig, (uint32_t)(sizeof(zbig)));
    }
    initramfs_file("bigdemo",   /* v0.26#3: >64KB 大 ELF（验证加载去上限） */
                   _binary_bigdemo_elf_start,
                   (uint32_t)(_binary_bigdemo_elf_end - _binary_bigdemo_elf_start));
    /* v0.27 工具链：编译器 ELF + 其自举源码（guest 内写-编-跑闭环的素材） */
    initramfs_file("cc500",
                   _binary_cc500_elf_start,
                   (uint32_t)(_binary_cc500_elf_end - _binary_cc500_elf_start));
    initramfs_file("cc500.c",
                   _binary_cc500_c_start,
                   (uint32_t)(_binary_cc500_c_end - _binary_cc500_c_start));
    /* V1 自研编译器（MIT）：guest 内经 shell `micc <src> <out>` 编译运行（int-only 子集） */
    initramfs_file("minicc",
                   _binary_minicc_elf_start,
                   (uint32_t)(_binary_minicc_elf_end - _binary_minicc_elf_start));
    /* V3 自举：P1（minicc-self）+ 其源码（minicc.c）——shell `miccboot` 验证 P1==P2 */
    initramfs_file("minicc-self",
                   _binary_minicc_self_elf_start,
                   (uint32_t)(_binary_minicc_self_elf_end - _binary_minicc_self_elf_start));
    initramfs_file("minicc.c",
                   _binary_minicc_self_c_start,
                   (uint32_t)(_binary_minicc_self_c_end - _binary_minicc_self_c_start));
    initramfs_file("shell",
                   _binary_shell_elf_start,
                   (uint32_t)(_binary_shell_elf_end - _binary_shell_elf_start));
    /* v1.1 Step 4：虚拟 TCP 薄包装 HTTP demo（开机在 kernel.c 按 TCP_DEMO spawn；此处仅入 initramfs，
     * shell `run httpdemo` 亦可触发） */
    initramfs_file("httpdemo",
                   _binary_httpdemo_elf_start,
                   (uint32_t)(_binary_httpdemo_elf_end - _binary_httpdemo_elf_start));
    /* v1.2 大文件下载 demo（开机按 DL_DEMO spawn；亦可 shell `run dldemo` 触发） */
    initramfs_file("dldemo",
                   _binary_dldemo_elf_start,
                   (uint32_t)(_binary_dldemo_elf_end - _binary_dldemo_elf_start));
    /* v1.5 P2 编译硬化：栈金丝雀演示（shell `run cansmash`）*/
    initramfs_file("cansmash",
                   _binary_cansmash_elf_start,
                   (uint32_t)(_binary_cansmash_elf_end - _binary_cansmash_elf_start));
    /* v0.34 BUG-058 per-process syscall 掩码演示（shell `run sandboxdemo`）*/
    initramfs_file("sandboxdemo",
                   _binary_sandboxdemo_elf_start,
                   (uint32_t)(_binary_sandboxdemo_elf_end - _binary_sandboxdemo_elf_start));
    /* SEC-01 回归：ring3 非法指令探针（shell `run badinsn` 触发 #UD，验系统存活）*/
    initramfs_file("badinsn",
                   _binary_badinsn_elf_start,
                   (uint32_t)(_binary_badinsn_elf_end - _binary_badinsn_elf_start));
    /* 加固 A-1 ④：ring3 随机坏指令流探针（shell `run chaos` fork N 轮仅杀子进程）*/
    initramfs_file("chaos",
                   _binary_chaos_elf_start,
                   (uint32_t)(_binary_chaos_elf_end - _binary_chaos_elf_start));
#ifdef GUEST_DIFF
    /* V4 mini-Csmith guest 差分：批量嵌入随机样本，shell `run dsXX` 取语义退码 */
    initramfs_file("ds00", _binary_ds00_elf_start, (uint32_t)(_binary_ds00_elf_end - _binary_ds00_elf_start));
    initramfs_file("ds01", _binary_ds01_elf_start, (uint32_t)(_binary_ds01_elf_end - _binary_ds01_elf_start));
    initramfs_file("ds02", _binary_ds02_elf_start, (uint32_t)(_binary_ds02_elf_end - _binary_ds02_elf_start));
    initramfs_file("ds03", _binary_ds03_elf_start, (uint32_t)(_binary_ds03_elf_end - _binary_ds03_elf_start));
    initramfs_file("ds04", _binary_ds04_elf_start, (uint32_t)(_binary_ds04_elf_end - _binary_ds04_elf_start));
    initramfs_file("ds05", _binary_ds05_elf_start, (uint32_t)(_binary_ds05_elf_end - _binary_ds05_elf_start));
    initramfs_file("ds06", _binary_ds06_elf_start, (uint32_t)(_binary_ds06_elf_end - _binary_ds06_elf_start));
    initramfs_file("ds07", _binary_ds07_elf_start, (uint32_t)(_binary_ds07_elf_end - _binary_ds07_elf_start));
    initramfs_file("ds08", _binary_ds08_elf_start, (uint32_t)(_binary_ds08_elf_end - _binary_ds08_elf_start));
    initramfs_file("ds09", _binary_ds09_elf_start, (uint32_t)(_binary_ds09_elf_end - _binary_ds09_elf_start));
    initramfs_file("ds10", _binary_ds10_elf_start, (uint32_t)(_binary_ds10_elf_end - _binary_ds10_elf_start));
    initramfs_file("ds11", _binary_ds11_elf_start, (uint32_t)(_binary_ds11_elf_end - _binary_ds11_elf_start));
#endif
}

/* ---- BUG-057 P3（审计跟进）：持久化老盘的只读补齐 ----
 * initramfs 重建路径（无盘 + 有盘首启格式化）在写文件后已 fs_protect；
 * 但"有效 FS 直接挂载"路径跳过 initramfs，老镜像系统文件 mode=0 仍可删可改——
 * 启动时对系统文件清单幂等 fs_protect 一次，老盘升级首启即补齐保护、无缝迁移。
 * fs_protect 幂等（mode |= FS_MODE_RO）；清单中缺失的文件（旧盘无此类）走 fs_lookup
 * 失败返回 -1 无害，不要求重新格式化/save。 */
static const char *const sysfiles[] = {
    "motd",
    "hello", "echo", "crash", "isol", "forkdemo", "args", "stackovf",
    "deep", "deepfork", "deepexec", "heapdemo", "fsdemo", "waitdemo",
    "abuse", "sockdemo", "zbig", "bigdemo", "cc500", "cc500.c",
    "minicc", "minicc-self", "minicc.c",
    "shell", "httpdemo", "dldemo", "cansmash", "sandboxdemo",
};
#define SYSFILES_COUNT (sizeof(sysfiles) / sizeof(sysfiles[0]))

static void storage_protect_sysfiles(void) {
    for (uint32_t i = 0; i < SYSFILES_COUNT; i++)
        fs_protect(fs_device(), sysfiles[i]);
}

void storage_init(void) {
    uint32_t phys = frame_alloc_run(RAMDISK_BLOCKS);   /* 申请连续物理帧 */
    if (!phys) {
        serial_puts("[storage] FATAL: ramdisk alloc failed\n");
        vga_puts("[storage] FATAL: ramdisk alloc failed\n");
        for (;;);
    }
    blockdev_init(&ramdisk, (uint8_t *)phys, RAMDISK_BLOCKS);

    if (ata_sectors() > 0) {          /* 真盘存在 */
        disk_load();
        uint32_t magic = *(uint32_t *)blockdev_ptr(&ramdisk, 0, 0);
        fs_mount(&ramdisk);
        if (magic == FS_MAGIC) {      /* 磁盘已有有效 FS：直接挂载，跳过格式化/initramfs */
            persistent = 1;
            /* RD5（BUG-071）：外部镜像挂载即重建"块→owner"归属账本，扫描在用 inode 全部块；
             * 检测"合法范围内重复块"（两 inode 共享同一数据块，含 RO 文件块被别名）并计数。 */
            fs_scan_owners(&ramdisk);
            serial_printf("[storage] persistent FS mounted (magic=%x) @%x\n", magic, phys);
            vga_printf("[storage] persistent FS mounted (magic=%x)\n", magic);
            /* BUG-057 P3：老盘挂载补齐系统文件只读。幂等、缺文件无害；补齐后落盘一次，
             * 使 RO 位持久化（否则重启 disk_load 读回 mode=0 保护即失效）。 */
            storage_protect_sysfiles();
            disk_save();
            return;
        }
        serial_puts("[storage] disk blank -> format + initramfs\n");
        vga_puts("[storage] disk blank -> format + initramfs\n");
        int rc = fs_init(&ramdisk);
        serial_printf("[storage] format %s\n", rc == 0 ? "ok" : "FAIL");
        vga_printf("[storage] format %s\n", rc == 0 ? "ok" : "FAIL");
        initramfs_setup();
        persistent = 1;
        disk_save();                  /* 首启落盘一次，磁盘立即具备有效镜像 */
        return;
    }

    /* 无盘：纯内存盘（v0.8 原行为，重启丢失） */
    fs_mount(&ramdisk);
    int rc = fs_init(&ramdisk);
    serial_printf("[storage] ramdisk %u blocks @%x (%uKB), format %s\n",
                  RAMDISK_BLOCKS, phys, RAMDISK_BLOCKS * 4u, rc == 0 ? "ok" : "FAIL");
    vga_printf("[storage] ramdisk %u blocks @%x (%uKB), format %s\n",
               RAMDISK_BLOCKS, phys, RAMDISK_BLOCKS * 4u, rc == 0 ? "ok" : "FAIL");
    initramfs_setup();
}
