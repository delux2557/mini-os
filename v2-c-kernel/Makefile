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
# 编译硬化门禁（P1）：CI 固定 gcc13 下把告警升级为错误，防新增代码告警回退。
# 默认开；本地临时豁免用 `make WERROR=`（空串）关闭——注意是"默认开、显式传空串关"，
# 门禁性质的变量不当默认关。cc500 自有 -w/-Wno-int-conversion 豁免，与 -Werror 不冲突。
WERROR  ?= -Werror
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector \
          -fno-builtin -fno-asynchronous-unwind-tables -fcf-protection=none \
          -Wall -Wextra $(WERROR) -O2 -std=gnu99 -nostdlib \
          -MMD -MP \
          -Isrc -Isrc/arch -Isrc/kernel -Isrc/mm -Isrc/drv -Isrc/fs -Isrc/net -Isrc/app -I$(BUILD)
# ---- P2 编译硬化（教学增强）：用户态 app 栈金丝雀 ----
#  - 只对 src/app/ 的用户态程序生效，内核 src/ 保持 -fno-stack-protector（CFLAGS 已含）。
#  - 32 位无 TLS，必须 -mstack-protector-guard=global 走全局变量取金丝雀（否则 gcc 用 GS 段会崩）。
#  - 该 flag 只是叠加在 CFLAGS 之后，gcc 命令行里最后的 -f*stack-protector 生效。
#  - __stack_chk_guard / __stack_chk_fail 由 crt.c 提供（每个 app ELF 链 crt.o 均可得）。
APP_SEC_CFLAGS = -fstack-protector-all -mstack-protector-guard=global
ASM     = nasm
LD      = ld
LDFLAGS = -m elf_i386 -T src/linker.ld -nostdlib -z noexecstack
OBJCOPY = objcopy

# 源文件查找路径（按子系统；同名文件在各目录间保持唯一）
VPATH = src:src/arch:src/kernel:src/mm:src/drv:src/fs:src/net:src/app

# 构建/录制产物根目录。
#  （BUG-053 修复：默认仍为 build/，但允许 `make BUILD=<私有目录>` 覆盖——
#   并发重负荷 harness 各用私有目录，避免共享 build/ 下 `make clean` 互踩金标/transcript。）
# 注意：tests/*.sh 内对 `build/` 的引用当前仍为字面路径，全量隔离 wiring 见 bugs.md BUG-053。
BUILD        ?= build
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
       e1000_netif.o netif.o slip.o uart_netif.o \
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
# v1.1 Step 2：UART_NETIF_DEFAULT 非空时，内核编译加 -D：netif 静态绑定串口网卡为当前
#（tests/test_slip_net.sh 用 `make UART_NETIF_DEFAULT=1` 构建，跑完 `make clean && make` 恢复）。
# v1.1 Step 4：TCP_DEMO 非空时加 -D，开机自动 spawn httpdemo（虚拟 TCP 演示，
# tests/test_tcp.sh 用它，跑完恢复）。
ifdef UART_NETIF_DEFAULT
UART_NETIF_DEFAULT_DEF = -DUART_NETIF_DEFAULT
endif
ifdef TCP_DEMO
TCP_DEMO_DEF = -DTCP_DEMO
endif
# v1.2 BUG-047 大文件下载：DL_DEMO 非空时加 -D，开机自动 spawn dldemo 拉 128KB
#（tests/test_tcp_dl.sh 用它，跑完恢复）。
ifdef DL_DEMO
DL_DEMO_DEF = -DDL_DEMO
endif
$(BUILD)/kernel.o: src/kernel/kernel.c $(USERPROG_OFFS)
	$(CC) $(CFLAGS) $(UART_NETIF_DEFAULT_DEF) $(TCP_DEMO_DEF) $(DL_DEMO_DEF) -c $< -o $@

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
#   - 普通应用链接到 APP_ADDR（0x800A0000，app 槽），退出即回收
# SEC-01 回归：badinsn 以 ring3 执行 ud2（#UD），验证用户态 CPU 异常被隔离杀进程而非整机停机
APP_ADDR   = 0x800A0000
SHELL_ADDR = 0x80090000
APPS       = hello echo crash isol forkdemo args stackovf deep deepfork deepexec heapdemo fsdemo waitdemo abuse sockdemo bigdemo cc500 httpdemo dldemo cansmash sandboxdemo badinsn
APP_ELFOBJS  = $(addprefix $(BUILD)/apps/, $(addsuffix _elf.o, $(APPS)))
SHELL_ELFOBJ = $(BUILD)/apps/shell_elf.o

# 编译（源文件在 src/app/；构建输出仍在 build/apps/）
# P2：普通 app 追加 canary flags；cc500.o/cc500_crt.o 走下方独立显式规则豁免（见 v0.27 段）。
$(BUILD)/apps/%.o: src/app/%.c
	$(CC) $(CFLAGS) $(APP_SEC_CFLAGS) -c $< -o $@

# 链接到固定地址（-e _start 指定 ELF 入口；crt.o 提供 _start：app_main 返回即 sys_exit）
$(BUILD)/apps/hello.elf: $(BUILD)/apps/hello.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/echo.elf: $(BUILD)/apps/echo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/crash.elf: $(BUILD)/apps/crash.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/badinsn.elf: $(BUILD)/apps/badinsn.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/isol.elf: $(BUILD)/apps/isol.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/forkdemo.elf: $(BUILD)/apps/forkdemo.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/args.elf: $(BUILD)/apps/args.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/stackovf.elf: $(BUILD)/apps/stackovf.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/cansmash.elf: $(BUILD)/apps/cansmash.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $<
$(BUILD)/apps/sandboxdemo.elf: $(BUILD)/apps/sandboxdemo.o $(BUILD)/apps/crt.o
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
# cc500 专用 CRT：与 cc500.o 同属工具链，显式豁免 canary（APP_SEC_CFLAGS）。
# 关键：cc500.elf 只链接 cc500_crt.o + cc500.o（无 crt.o 的 __stack_chk_* 符号），
# 若这里也带 canary，会因未定义 __stack_chk_guard 链接失败。显式规则优先于 apps 模式规则。
$(BUILD)/apps/cc500_crt.o: src/app/cc500_crt.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/apps/cc500.elf: $(BUILD)/apps/cc500.o $(BUILD)/apps/cc500_crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/cc500_crt.o $<

# ---- netif Step 4：httpdemo（虚拟 TCP 薄包装演示）。薄包装 lib（src/app/tcp.c）
# 与演示共享同一套 CFLAGS（-Isrc/net 已含 tcp_proto.h），链接进 httpdemo.elf ----
$(BUILD)/apps/httpdemo.elf: $(BUILD)/apps/httpdemo.o $(BUILD)/apps/tcp.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $(BUILD)/apps/tcp.o $<
# v1.2 大文件下载 demo：与 httpdemo 同套虚拟 TCP 薄包装 lib（tcp.o）
$(BUILD)/apps/dldemo.elf: $(BUILD)/apps/dldemo.o $(BUILD)/apps/tcp.o $(BUILD)/apps/crt.o
	$(LD) -m elf_i386 -Ttext $(APP_ADDR) -e _start -o $@ $(BUILD)/apps/crt.o $(BUILD)/apps/tcp.o $<
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

# v0.31 socket 归属回归：F-0a 退出泄漏回收 / F-0b 保留槽防任意 close / 观测收口
test-socket:
	bash tests/test_socket.sh

# v0.32 cc500 编译器回归：F-1 关系运算残缺 / F-2 未定义符号静默 / F-3 未闭合字符串自噬
test-cc500:
	bash tests/test_cc500.sh

# v1.1 Step 2 串口网卡回归：COM2 + SLIP（RFC 1055）UDP 数据报与宿主对端往返一致
test-slip:
	bash tests/test_slip_net.sh

# v1.1 Step 4 虚拟 TCP 回归：宿主转发器（UDP 通道）+ HTTP 请求-响应 demo + 断连/超时/-1 断言
test-tcp:
	bash tests/test_tcp.sh

# record/replay 地基 P1：两次 -icount 冷启动串口日志逐字节 diff（确定性验收，非网络层）
test-det:
	bash tests/test_determinism.sh

# record/replay 地基 P2：transcript 固化——输入/输出固化 + 失败自动归档 + 复现性雏形
test-tr:
	bash tests/test_transcript.sh

# BUG-052 稳定性回归：test-tr 连跑 N 次（默认 3），任一次失败即 exit 1。
# 意图：加固"0 假红"判据——单跑偶发放过因串口撕裂/捕获窗引发的 flake，
# 连跑把这层"judging 信噪比"的老毛病显性化（修复前 ~40-67% flake）。
# 慢（多次 QEMU 冷启），故仅挂在非 required 的 regression-rr job；`make test` 仍单跑。
TR_STA_RUNS ?= 3
test-tr-stable:
	@for i in $$(seq 1 $(TR_STA_RUNS)); do echo "test-tr run#$$i/$(TR_STA_RUNS)..."; bash tests/test_transcript.sh || { echo "[FAIL] test-tr run#$$i flake/回归"; exit 1; }; done; echo "[ok] test-tr $(TR_STA_RUNS)/$(TR_STA_RUNS) pass (0 假红)"

# record/replay 地基 P3：回放差分——消费 transcript 两次重放差分一致 + 已修 bug 触发链路
test-rp:
	bash tests/test_replay.sh

# record/replay 接 repro_bugs.sh：BUG-A(文件槽泄漏)/BUG-B(cc500 argv) 复现命令流经 transcript
# 录制固化为可回放证据（.in.tr/.out.tr），并断言两 bug 在修复版"未复现"（含 -nic none）。
# 录制的 transcript 可被 replay.sh 消费重放（P3 方向，闭环见 docs）。
# F6（交接单 处理项）：BUG-B 断言已由软分支改硬断言（按 `[ls] out2.elf` 目录条目判定 argv 生效）。
test-repro:
	bash tests/repro_bugs.sh

# 虚拟 TCP 压力/攻击回归（F3 处理项落点）：脏注入 + 合法业务完整交付。
# 起服务前端口占用检测，命中即 exit 2（环境病），不误杀他人进程。
test-tcp-attack:
	bash tests/test_tcp_attack.sh

# SEC-07 内核栈预算门禁：DHCP 续约 IRQ0 链栈帧总和断言（tests/check_stack_budget.sh）。
# -fstack-usage 是纯分析（不改代码生成），只对链上几个文件单独开启并产出 .su，
# 不进全局 CFLAGS；秒级，故并进 make test 主链。
STACK_SU     = $(BUILD)/stackbudget
# L1（栈预算总账）：除 SEC-07 网卡链外，新增 syscall 分发器 / 调度 / 键盘 / 串口 /
# 定时器文件，供 check_stack_budget.sh 对全部"关键根链"做帧预算断言。
STACK_FILES  = e1000.o netsock.o netif.o e1000_netif.o usermode.o sched.o timer.o kb.o serial.o
STACK_OBJS   = $(addprefix $(STACK_SU)/, $(STACK_FILES))
$(STACK_SU)/%.o: %.c
	@mkdir -p $(STACK_SU)
	$(CC) $(CFLAGS) -fstack-usage -c $< -o $@
test-stack: $(STACK_OBJS)
	bash tests/check_stack_budget.sh $(STACK_SU) 3584

# 完整测试
test: test-host test-qemu test-serial test-persist test-net test-socket test-cc500 test-slip test-tcp test-det test-tr test-rp test-stack

# ---- 编译硬化门禁（P1）：危险 libc 调用检查 ----
# 审计已确认全库无 strcpy/strcat/sprintf/vsprintf/gets；此目标把"人工检查"固化为可执行门禁，
# 防后续新增代码引入危险函数。范围 src/ 足够（cc500 在 tools/，不扩扫）。
# lint 记录：
#   - 用 -w 词边界，避免未来误伤 snprintf（安全）、getsomething 之类标识符子串；
#   - user_lib.h 里无危险函数，负过滤保留只是防御性写法。
lint:
	@echo "[lint] checking banned libc..."
	@if grep -Rrnw 'src/' -e 'strcpy' -e 'strcat' -e 'sprintf' -e 'vsprintf' -e 'gets' \
	     --include="*.c" --include="*.h" | grep -v 'user_lib.h'; then \
	  echo "[lint] ERROR: banned libc function found"; exit 1; fi
	@echo "[lint] OK"

clean:
	rm -rf $(BUILD) tests/build

# v0.26 bugfix: 头文件变更自动触发重编（-MMD 生成的 .d 依赖）
-include $(BUILD)/*.d

.PHONY: run run-serial test test-host test-qemu test-serial test-persist test-net test-socket test-cc500 test-slip test-tcp test-det test-tr test-tr-stable test-rp test-repro test-tcp-attack test-stack lint clean
