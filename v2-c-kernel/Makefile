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
          -MMD -MP \
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
       brk.o usermode.o sched.o sched_policy.o sem.o msg.o blockdev.o fs.o elf.o userptr.o \
       pci.o e1000.o netutil.o ip.o udp.o icmp.o dhcp.o netsock.o \
       ata.o storage.o userprog_bin.o) \
       $(APP_ELFOBJS) $(SHELL_ELFOBJ)
# v0.27: cc500 编译器源码 blob（嵌入 initramfs 作 /cc500.c，供 guest 内自举）。
# 必须在 $(KERNEL): $(OBJS) 规则之前 append，否则该规则解析时（prereq 即时展开）
# 不会包含它，导致 make 不先构建 cc500_csrc.o（recipe 因延迟展开却会引用它）。
CC500_CSRC = $(BUILD)/apps/cc500_csrc.o
OBJS += $(CC500_CSRC)

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# ---- 汇编 / C（默认模式规则，源文件经 VPATH 定位） ----
$(BUILD)/%.o: %.s
	$(ASM) -f elf32 $< -o $@

$(BUILD)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- v0.28 DHCP 租期续约：测试用短租期覆盖 ----
# DHCP_RENEW_SECS 非空时，e1000 编译加 -D：把 DHCP 服务器租期覆盖为该值（秒），
# 使 T1/T2 在秒级窗口内触发续约闭环（tests/test_net.sh 用 `make DHCP_RENEW_SECS=2`
# 构建，跑完再 `make clean && make` 恢复常规内核）。缺省用服务器租期（SLIRP 24h）。
ifdef DHCP_RENEW_SECS
DHCP_RENEW_DEF = -DDHCP_RENEW_SECS=$(DHCP_RENEW_SECS)
endif
$(BUILD)/e1000.o: src/drv/e1000.c
	$(CC) $(CFLAGS) $(DHCP_RENEW_DEF) -c $< -o $@

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
#  - 普通应用链接到 APP_ADDR（0x800A0000，app 槽），退出即回收
#  - shell 链接到 SHELL_ADDR（0x80090000），内核启动时加载常驻
#  - 注意：嵌入的是 ELF 文件本身（用 ld -r -b binary），不能用 objcopy -O binary
#    （那会剥掉 ELF 头，内核的 elf_load 需要解析程序头）
APP_ADDR   = 0x800A0000
SHELL_ADDR = 0x80090000
APPS       = hello echo crash isol forkdemo args stackovf deep deepfork deepexec heapdemo fsdemo waitdemo abuse sockdemo bigdemo cc500
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
$(BUILD)/apps/deep.elf: $(BUILD)/apps/deep.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/deepfork.elf: $(BUILD)/apps/deepfork.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/deepexec.elf: $(BUILD)/apps/deepexec.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/heapdemo.elf: $(BUILD)/apps/heapdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/fsdemo.elf: $(BUILD)/apps/fsdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/waitdemo.elf: $(BUILD)/apps/waitdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/abuse.elf: $(BUILD)/apps/abuse.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/sockdemo.elf: $(BUILD)/apps/sockdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/bigdemo.elf: $(BUILD)/apps/bigdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/shell.elf: $(BUILD)/apps/shell.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(SHELL_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<

# ---- v0.27 工具链：cc500 编译器 ----
#  - 源码在 tools/cc500/cc500.c（自举源，同时嵌入 initramfs 作 /cc500.c）
#  - 用专用 cc500_crt.o（_start 以 cc500_main 返回值 sys_exit），普通 crt 会吞掉退出码
#  - 该源受"CC500 语言子集"约束（无类型转换），syscall3 指针/整数混参是子集固有
#    写法，GCC>=14 把 -Wint-conversion 设为 permerror（硬错误）会误杀；故文件级豁免。
#  - `-Wno-int-conversion`（必需）：GCC 14 中它是 permerror，`-w` 只抑制 warning 压不住
#    error（BUG-036：v0.30 曾误删此项只留 -w，GCC 14 下 make 直接 EXIT=2 构建失败；
#    GCC 13 下 -w 恰好够用是旧假设的来源）。
#  - `-w`：抑制其余普通警告（含 `in_data == (0-1)` 指针与整数比较的
#    -Wcompare-distinct-pointer-types 惯用法、-Wmaybe-uninitialized 等），
#    该源本就不能在源码层修复，只能豁免。GCC 8+ 两者均有效、可并存。
$(BUILD)/apps/cc500.o: tools/cc500/cc500.c
	$(CC) $(CFLAGS) -Wno-int-conversion -w -c $< -o $@
$(BUILD)/apps/cc500.elf: $(BUILD)/apps/cc500.o $(BUILD)/apps/cc500_crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/cc500_crt.o $<
# 源码以原始字节嵌入（符号 _binary_cc500_c_start/end，由输入文件名 cc500.c 决定）
$(BUILD)/apps/cc500.c: tools/cc500/cc500.c
	cp $< $@
$(BUILD)/apps/cc500_csrc.o: $(BUILD)/apps/cc500.c
	cd $(BUILD)/apps && $(OBJCOPY) -I binary -O elf32-i386 -B i386 cc500.c cc500_csrc.o

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

# v0.26 bugfix: 头文件变更自动触发重编（-MMD 生成的 .d 依赖）
-include $(BUILD)/*.d

.PHONY: run run-serial test test-host test-qemu test-serial test-persist test-net clean
