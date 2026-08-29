# mini-os/v2-c-kernel/Makefile
# 目录约定（按子系统分组，Linux/MINIX 风格）：
#   src/arch/     CPU/中断/时钟（boot.s isr.s idt timer）
#   src/kernel/   核心内核（kernel.c sched sem msg usermode userptr guard elf）
#   src/mm/       内存管理（mem heap）
#   src/drv/      设备驱动（vga serial kb ata pci e1000）
#   src/fs/       文件系统（fs blockdev storage）
#   src/net/      网络协议（netutil；v0.19 起 ip/udp，v0.23 加 icmp）
#   src/app/      用户程序（userprog.c user_lib.h + 各 ELF 应用）
#   src/*.ld      链接脚本（linker.ld userprog.ld）
#   tests/        宿主单测 + QEMU 回归脚本
#   build/        全部构建产物（make clean 一键清理，勿提交到 git）
# 构建技巧：源文件查找用 VPATH（子目录），头文件用 -I 逐目录找，
#          因此 #include "xxx.h" 与 OBJS 里的裸文件名都无需改。
CC      = gcc
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector \
          -fno-builtin -fno-asynchronous-unwind-tables -fcf-protection=none \
          -Wall -Wextra -O2 -std=gnu99 -nostdlib \
          -Isrc -Isrc/arch -Isrc/kernel -Isrc/mm -Isrc/drv -Isrc/fs -Isrc/net -Isrc/app -Ibuild
ASM     = nasm
LD      = ld
LDFLAGS = -m elf_i386 -T src/linker.ld -nostdlib -z noexecstack
OBJCOPY = objcopy

# 源文件查找路径（按子系统；同名文件在各目录间保持唯一）
VPATH = src:src/arch:src/kernel:src/mm:src/drv:src/fs:src/net:src/app

BUILD        = build
KERNEL       = $(BUILD)/kernel.elf
USERPROG_ELF = $(BUILD)/userprog.elf
USERPROG_BIN = $(BUILD)/userprog.bin
USERPROG_OBJ = $(BUILD)/userprog_bin.o
USERPROG_OFFS = $(BUILD)/userprog_offsets.h

# make 解析时确保输出目录存在
$(shell mkdir -p $(BUILD) $(BUILD)/apps)

# 内核目标文件（用户程序以二进制 .o 嵌入）
OBJS = $(addprefix $(BUILD)/, \
       boot.o kernel.o vga.o serial.o idt.o isr.o timer.o kb.o mem.o guard.o heap.o \
       usermode.o sched.o sched_policy.o sem.o msg.o blockdev.o fs.o elf.o userptr.o \
       pci.o e1000.o netutil.o ip.o udp.o icmp.o dhcp.o netsock.o \
       ata.o storage.o userprog_bin.o) \
       $(APP_ELFOBJS) $(SHELL_ELFOBJ)

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# ---- 汇编 / C（默认模式规则，源文件经 VPATH 定位） ----
$(BUILD)/%.o: %.s
	$(ASM) -f elf32 $< -o $@

$(BUILD)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 依赖自动生成的头文件（userprog 各进程入口偏移）
$(BUILD)/kernel.o: src/kernel/kernel.c $(USERPROG_OFFS)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sched.o: src/kernel/sched.c $(USERPROG_OFFS)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- 用户程序：编译 -> 链接(0x80000000) -> 转二进制 -> 以 .o 嵌入内核 ----
$(BUILD)/userprog.o: src/app/userprog.c
	$(CC) $(CFLAGS) -c $< -o $@

$(USERPROG_ELF): $(BUILD)/userprog.o src/userprog.ld
	$(LD) -m elf_i386 -T src/userprog.ld -o $@ $(BUILD)/userprog.o

$(USERPROG_BIN): $(USERPROG_ELF)
	$(OBJCOPY) -O binary $< $@

$(USERPROG_OBJ): $(USERPROG_BIN)
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf32-i386 -B i386 userprog.bin userprog_bin.o

# 提取各进程入口相对 0x80000000 的偏移（user_main_a/b, user_sem_a/b, user_msg_p/c, user_fs_w/l, user_crash）
$(USERPROG_OFFS): $(USERPROG_ELF)
	nm $< | awk '/ (user_main_a|user_main_b|user_sem_a|user_sem_b|user_msg_p|user_msg_c|user_fs_w|user_fs_l|user_crash)$$/ { \
	    sym = $$3; sub(/^user_main_/, "MAIN_", sym); sub(/^user_/, "", sym); \
	    print "#define USER_" toupper(sym) "_OFF 0x" substr($$1,5) }' > $@

# ---- v0.9 应用：独立 ELF，链接到固定地址，整体（含 ELF 头）内嵌到内核 ----
#  - 普通应用链接到 APP_ADDR（0x80040000，app 槽），退出即回收
#  - shell 链接到 SHELL_ADDR（0x80030000），内核启动时加载常驻
#  - 注意：嵌入的是 ELF 文件本身（用 ld -r -b binary），不能用 objcopy -O binary
#    （那会剥掉 ELF 头，内核的 elf_load 需要解析程序头）
APP_ADDR   = 0x80040000
SHELL_ADDR = 0x80030000
APPS       = hello echo crash isol forkdemo args stackovf fsdemo waitdemo abuse sockdemo
APP_ELFOBJS  = $(addprefix $(BUILD)/apps/, $(addsuffix _elf.o, $(APPS)))
SHELL_ELFOBJ = $(BUILD)/apps/shell_elf.o

# 编译（源文件在 src/app/；构建输出仍在 build/apps/）
$(BUILD)/apps/%.o: src/app/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# 链接到固定地址（-e _start 指定 ELF 入口；crt.o 提供 _start：app_main 返回即 sys_exit）
$(BUILD)/apps/hello.elf: $(BUILD)/apps/hello.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/echo.elf: $(BUILD)/apps/echo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/crash.elf: $(BUILD)/apps/crash.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/isol.elf: $(BUILD)/apps/isol.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/forkdemo.elf: $(BUILD)/apps/forkdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/args.elf: $(BUILD)/apps/args.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/stackovf.elf: $(BUILD)/apps/stackovf.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/fsdemo.elf: $(BUILD)/apps/fsdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/waitdemo.elf: $(BUILD)/apps/waitdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/abuse.elf: $(BUILD)/apps/abuse.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/sockdemo.elf: $(BUILD)/apps/sockdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/shell.elf: $(BUILD)/apps/shell.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(SHELL_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<

# ELF 文件原始字节内嵌（objcopy -I binary 按原始字节复制，保留 ELF 头；
# 符号名 _binary_<name>_elf_start/end，由输入文件名决定）
$(BUILD)/apps/%_elf.o: $(BUILD)/apps/%.elf
	cd $(BUILD)/apps && $(OBJCOPY) -I binary -O elf32-i386 -B i386 $*.elf $*_elf.o

# kernel 依赖应用 ELF blob，应用改动后自动重链
$(BUILD)/kernel.o: $(APP_ELFOBJS) $(SHELL_ELFOBJ)

# 带图形界面运行（本地桌面环境）
run: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL)

# 无图形界面运行，内核输出通过串口写到 build/serial.log
run-serial: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -display none -serial file:$(BUILD)/serial.log -no-reboot

# ---- 测试 ----
# 宿主单元测试：纯逻辑编译成普通 Linux 程序运行（秒级、可加 ASan）
test-host:
	bash tests/run_host_tests.sh

# QEMU 自动回归：无图形界面运行 + 串口日志关键字校验
test-qemu:
	bash tests/qemu_regression.sh

# QEMU 串口终端回归：模拟外部 agent 经 `-serial stdio` 驱动 shell（IRQ4 接收通道）
test-serial:
	bash tests/test_serial.sh

# QEMU ATA 真盘持久化回归：两次运行共享磁盘镜像，校验用户数据跨重启存活
test-persist:
	bash tests/test_persist.sh

# QEMU 网络回归：e1000 驱动 + ARP 与 SLIRP 网关端到端互通（串口日志 + pcap 独立核验）
test-net:
	bash tests/test_net.sh

# 完整测试
test: test-host test-qemu test-serial test-persist test-net

clean:
	rm -rf build tests/build

.PHONY: run run-serial test test-host test-qemu test-serial test-persist test-net clean
