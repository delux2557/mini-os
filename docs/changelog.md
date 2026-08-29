# 版本变更日志（Changelog）

> 格式遵循 Keep a Changelog 精神：每个版本列出 Added / Changed / Fixed / Engineering。

## [v0.12] - 2026-08-29 · fork/exec 进程模型与 argv 参数传递

**Added**
- `sys_fork()`（SYS_FORK=24）：复制当前进程——用户地址空间**深拷贝**（逐页分配新物理帧并拷贝内容），
  共享内存区（`SHMEM_VBASE`）保持共享；父进程返回子 pid，子进程从 fork 调用点继续（返回 0）
- `sys_exec(name, argc, argv)`（SYS_EXEC=25）：镜像替换——加载 ELF 到新地址空间，释放旧地址空间，
  复用当前 pid 与内核栈，按 cdecl 在新用户栈布置 argv 块后切入新程序入口
- **pid 槽位重用**：`alloc_pid()` 扫描空闲 PCB 槽，进程退出后 pid 可复用
  （单调递增的 next_pid 会在并发演示（fork/isol 等）耗尽 MAX_PROCS 时无法再创建）
- 应用入口统一为 `app_main(int argc, char **argv)`：内核以 cdecl 进入
  （`[esp]=返回地址, [esp+4]=argc, [esp+8]=argv`），argv 数组与字符串布置在新用户栈顶
- 演示应用：
  - `forkdemo`：fork 父子分叉，双方把同一虚拟地址映射到不同物理页 → 双 `ISOLATED OK`
  - `args`：打印 argc 与每个 argv（argv[0]=程序名）
- shell 新增 `exec <prog> [args...]` 命令：**经典 fork+exec+argv+wait 全链路**
  （fork 子进程 → 子进程 exec → 父进程 wait）
- PCB 新增 `fork_frames/fork_fcount`：fork 深拷贝出的物理帧，退出时统一回收

**Fixed**
- BUG-010：`sched_exec` 释放旧地址空间后才 `set_name`，而 name 指向旧用户栈 → 缺页
- BUG-011：`sys_exec` 切 CR3 后 `load_elf_file(name)` 才读 name（旧用户栈）→ 缺页；
  name 与 argv 须在切 CR3 前拷入内核缓冲
- BUG-012：argv 的 cdecl 栈布局顺序错位（argc/argv 槽、argv 指针槽），多次修正后正确

**Engineering**
- QEMU 回归新增 v0.12 检查项：fork 父子分叉/子进程返回 0/深拷贝隔离、exec 镜像替换、
  argv 参数传递（argc=4、argv[1] 内容）、exec 退出码
- 串口终端回归补 `run forkdemo` 与 `exec args hello world` 用例
- Makefile 新增 `forkdemo`/`args` 应用构建与 initramfs 落盘
- 共享内存区布局常量移入 mem.h（fork 识别共享页跳过深拷贝）

## [v0.11] - 2026-08-29 · 每进程地址空间与物理内存隔离

**Added**
- 每进程独立地址空间 `mem.c/h`：`addr_space_create/destroy`（克隆内核共享 PDE + 清空用户半区）、
  `map_page_in`（映射到指定页目录）、`switch_page_dir`（写 CR3 自动刷 TLB）
- PCB 新增 `page_dir`：每个进程持有自己的页目录物理地址；idle/内核使用内核页目录（`page_dir=0`）
- 调度器上下文切换时同步切 CR3：`schedule()/sched_start()/sched_switch` 切到目标进程地址空间
- ELF 加载适配：`usermode_spawn_elf` 建独立地址空间 → 加载期间把 CR3 切到目标页目录直接写入
  段数据（不再临时映射进父进程页目录，避免覆盖父进程自身映射）
- 新系统调用 `sys_map_page(vaddr)`：用户进程在**自己的私有地址空间**申请物理页并映射
- 共享内存适配：`sys_shmem` 每次调用都重新映射共享物理帧进当前进程页目录（v0.11 起各进程页表不再共享）
- 隔离演示应用 `src/apps/isol.c`：两个并发实例把同一虚拟地址 `0x80050000` 映射到**不同物理页**，
  各自写入独立值并读回校验 → `ISOLATED OK`（物理内存隔离的铁证）
- PCB 新增 `map_frames/map_fcount`（用户经 sys_map_page 申请的物理页，退出时回收）
- PCB 新增 `name_buf[16]`：进程名拷入内核内存（父进程字符串位于其用户地址空间，子进程退出时 CR3 已切走，不能直接读）

**Fixed**
- BUG-009：引导期过早 `sti` 导致定时器抢占、shell 未注册即调度用户进程（见 bugs.md）
- GCC 14 构建可移植性：GCC 14 默认把 `-Wint-conversion`/`-Wincompatible-pointer-types`
  升级为编译错误——
  - `userprog.c` 52 处 `syscall3(SYS_PRINT, "字符串", ...)` 隐式指针→整数转换改为
    经 `sys_print()` 封装显式 `(uint32_t)` 窄化（与 user_lib.h 语义一致）
  - `serial.h` 的 `serial_rx_hook_t` 回调签名由 `void (*)(char)` 修正为 `int (*)(char)`
    （匹配 `kb_feed_char` 真实签名）
  - 已用 GCC 14.2.0 与 GCC 13.3.0 双编译器验证 `make test` 全绿

**Engineering**
- QEMU 回归新增 v0.11 检查项：isol 映射私有页、`ISOLATED OK`、两个实例落到 ≥2 个不同物理页
- 串口终端回归 `tests/test_serial.sh` 补 `run isol` 用例
- Makefile 新增 `isol` 应用构建与 initramfs 落盘

## [v0.10] - 2026-08-29 · 串口终端：外部 agent 经 QEMU 交互

**Added**
- 串口接收通道 `serial.c/h`：IRQ4 中断处理、`serial_rx_ready/getc/set_rx_hook`，
  接收中断到达后把 FIFO/缓冲内所有可用字符取走转发
- 输入源统一：`kb_feed_char(c)` 抽出"注入一个已解析 ASCII 字符"的公共路径
  （键盘查表结果或串口字符共用同一行缓冲），支持退格/回车/可打印字符
- `kernel.c` 把串口接收钩子接到键盘行缓冲：`serial_set_rx_hook(kb_feed_char)`
- PIC 掩码放开 IRQ4（`0xEC`）：`qemu -serial stdio` 即成为可交互的串口终端
- 终端回归脚本 `tests/test_serial.sh`：以 FIFO 管道模拟"外部 agent 通道"，
  经串口发送命令并校验输出（help/ls/cat motd/run hello/run echo/run crash），
  与 qemu_regression.sh（键盘 sendkey 路径）互补，验证"终端通道"

**Engineering**
- Makefile 新增 `test-serial` 目标并纳入 `test`（test-host + test-qemu + test-serial）
- `make run-serial` 可用 `-serial stdio` 直接交互，也可被外部 agent/工具驱动

## [v0.9] - 2026-08-29 · 可执行程序加载与交互式 Shell

**Added**
- ELF32 加载器 `elf.c/h`：解析程序头（PT_LOAD）、按链接地址加载、bss 清零、
  `mapfn` 钩子逐页申请物理帧并映射；`elf_load_range` 预计算页对齐的加载区间
- 应用独立编译为 ELF（`src/apps/`），链接到固定地址（普通应用 `0x80040000`、shell `0x80030000`），
  整体内嵌进内核，启动时作为 **initramfs** 写入 ramdisk（motd + hello/echo/crash/shell）
- 内核启动时从文件系统加载**常驻 shell**（`usermode_spawn_elf("shell", SHELL_LINK, resident=1)`，
  帧不随退出回收）
- 交互式 shell 应用 `src/apps/shell.c`：命令 `help / ls / cat <file> / run <prog> / exit`
  - `run <prog>`：`sys_spawn_file` 把 ELF 应用加载到 app 槽 → `sys_wait` 等其退出并打印退出码
- 新系统调用：`SYS_READLINE(20)`（阻塞式读一行）/ `SYS_SPAWN_FILE(21)`（从文件加载 ELF 建进程）/
  `SYS_WAIT(22)`（等待子进程退出，返回退出码）
- 用户应用：
  - hello：打印 pid/ticks 后退出（演示"从文件系统加载程序"）
  - echo：阻塞式 `readline` 读一行并回显（演示用户态阻塞 I/O）
  - crash：ring3 写内核显存 0xB8000 → 页错误 → 内核隔离终止（内存保护演示）
- 键盘行缓冲 `kb.c`：行缓冲与字符环形缓冲解耦、退格处理、行完成回调 `kb_set_line_hook`；
  进程阻塞在 `sys_readline` 上（`BLOCK_KEYBOARD`），行就绪时由 `sched_wake_keyboard` 唤醒并拷入
- 调度器扩展：`BLOCK_WAIT`（等子进程退出，exit 时唤醒并携带退出码）、`sched_spawn_at`、
  PCB 新增 `own_frames/own_fcount/own_vbase`（从文件加载的 ELF 代码帧，退出自动回收）
- 宿主单元测试：`tests/test_kb.c`（290 条：行缓冲/退格/越界/多次取行）、
  `tests/test_elf.c`（36 条：段加载/绝对寻址/bss 清零/mapfn 钩子/畸形输入/加载区间计算）、
  `tests/test_heap.c` 补 `frame_alloc_run` 多页分配用例

**Fixed**
- BUG-007：ELF 首段含 ELF 头所在页（`-Ttext` 地址的前一页）时，硬编码映射区间拒绝映射 → 拷贝缺页；
  改用 `elf_load_range` 动态计算 PT_LOAD 覆盖区间解决

**Engineering**
- Makefile 多 ELF 构建：`-Ttext` 固定地址 + `-e app_main` 指定 ELF 入口；
  `objcopy -I binary` 内嵌**完整 ELF 文件**（保留文件头供内核解析），区别于旧版 `objcopy -O binary`
- QEMU 回归升级为**交互式注入**：经 HMP monitor `sendkey` 注入键盘序列，
  端到端校验 shell 的 `help / ls / cat motd / run hello / run echo / run crash`
- 测试脚本同步基线：发送命令前记录日志行号，避免命中旧输出/漏掉同步写入

## [v0.8] - 2026-08-29 · 文件系统（内存盘 + 极简 mini-fs）

**Added**
- 块设备抽象 `blockdev.c/h`：以 4KB 块为单位的 read/write/ptr 接口，屏蔽后端差异；
  当前后端为内存盘（ramdisk，物理帧连续区，落在内核低 16MB 恒等映射区，可直接寻址）
- 极简文件系统 `fs.c/h`（类 Unix 磁盘布局）：
  - 块 0 超级块（magic "MINI"/总块数/inode 数）
  - 块 1 inode 位图、块 2 数据块位图、块 3 inode 表（64 个）、块 4.. 数据块
  - 只支持根目录、单级目录，文件名 <= 23 字符，直接块映射（单文件最大 12*4KB=48KB）
- 文件操作：`fs_init`（格式化）/`fs_create`/`fs_lookup`/`fs_delete`/`fs_read`/`fs_write`/`fs_list`，
  目录缺块自动扩容，写时按需分配数据块（新块清零），跨块读写自动切块
- 系统调用 13~19：`sys_fs_create/open/write/read/close/ls/delete`
  - 内核维护打开文件表 `fs_files[8]`（槽 0 保留），记录 inode/读写位置/模式（0 读 1 写）
  - 用户经固定槽位引用已打开文件，write/read 从当前位置推进（顺序 IO）
- 用户演示：两个新进程 procFSA/procFSB
  - procFSA：创建 hello.txt → 写入 8000 字节（跨块）→ 关闭 → 读回逐字节校验 → verify OK
  - procFSB：创建 alpha.txt/beta.txt → 写入 alpha.txt → 内核打印根目录列表（ls）→ 完成
- 宿主单元测试 `tests/test_fs.c`（1182 条断言：格式化/创建/重名拒绝/读写回读/
  跨块边界覆写/随机偏移抽查/删除后位图回收/inode 耗尽/目录扩容）

**Engineering**
- blockdev + fs 抽成纯逻辑模块（只依赖内存缓冲，不依赖内核/调度/硬件），宿主单测覆盖
- QEMU 回归新增 v0.8 检查项：内存盘初始化/进程 spawn/文件创建/写模式打开/跨块写入/
  读模式打开/读回校验通过/多文件创建/ls 列出/演示完成
- 用户程序新增 `.text.fs` 段（procFSA/procFSB 入口）；Makefile 加入 blockdev.o/fs.o

## [v0.7] - 2026-08-29 · IPC：有界消息队列（生产者-消费者）

**Added**
- 有界消息队列 `msg.c/h`：环形缓冲 + 双 FIFO 等待队列（生产者/消费者）
- 发送阻塞时**暂存消息**：`msg_send_try` 返回"应阻塞"并把 {pid, 消息} 入生产者队列，
  消费者取走后由 `msg_recv_wake` 把暂存消息搬入缓冲并唤醒（send 由内核代发，视为成功）
- 接收交棒语义：`msg_send_wake` 直接把刚入队的消息交付给等待消费者
  （消费者 recv 直接返回该消息，缓冲不滞留），保证消息恰好送达一次
- 调度器扩展：PCB 新增 `block_reason=BLOCK_MSG` 与 `block_arg` 字段；
  `sched_wake_with(pid, eax)` 支持唤醒时指定系统调用返回值（recv 返回消息值）
- 系统调用：`sys_msg_create(id, capacity)` / `sys_msg_send(id, value)` / `sys_msg_recv(id)`
- 用户演示：两个新进程 procMsgP/procMsgC
  - 消费者先建，立即在空缓冲上 recv 阻塞（recv-block）
  - 生产者快产慢消，塞满缓冲后 send 阻塞，由消费者取走唤醒（send-block）
  - 20 条消息 0..19 按序恰好一次送达，双方退出并被回收
- 宿主单元测试 `tests/test_msg.c`（117 条断言：环形回绕、双队列 FIFO、暂存/交棒、边界）

**Engineering**
- 消息队列抽成纯逻辑模块（无调度/硬件依赖），宿主单测覆盖
- QEMU 回归新增 v0.7 检查项：队列创建/消费者阻塞/生产者阻塞/生产者唤醒/收发完成
- 用户程序新增 `.text.msg` 段（procMsgP/procMsgC 入口）

## [v0.6] - 2026-08-29 · IPC 与同步（信号量 + 共享内存）

**Added**
- 信号量 `sem.c/h`：计数 + FIFO 等待队列；`sem_wait_try`（占用/应阻塞标记）、`sem_signal_wake`（唤醒队首/归还资源）
- 调度器扩展：`sched_block`（按原因阻塞当前进程）、`sched_wake`（唤醒并入就绪队列）、PCB 新增 `block_reason` 字段
- 系统调用：`sys_sem_create(id, init)` / `sys_sem_wait(id)` / `sys_sem_signal(id)` / `sys_shmem(slot)`
- 共享内存页：内核预映射共享物理帧到固定虚拟地址（所有进程共享页表，天然互通）
- 用户演示：两个新进程 procSemA/procSemB
  - **rendezvous 会合**：两个信号量双向等待，展示阻塞/唤醒（"arrived → rendezvous done"）
  - **互斥共享计数**：持锁后 sleep 强制对端在 `wait` 上阻塞，10 次自增最终恰为 10，无竞争丢失
- 宿主单元测试 `tests/test_sem.c`（68 条断言：计数增减、FIFO 顺序、满队列、资源守恒）

**Fixed**
- BUG-004：信号量等待者被定时器误唤醒（`sched_tick` 原只按 `wakeup_tick` 判阻塞）
- BUG-005：阻塞系统调用唤醒后 eax 返回值错误

**Engineering**
- 信号量抽成纯逻辑模块（无调度/硬件依赖），宿主单测覆盖
- QEMU 回归新增 v0.6 检查项：信号量创建/等待阻塞/唤醒/共享内存/rendezvous/互斥自增
- 内存布局扩展：共享页区 0x80020000（避开用户栈区）

## [v0.5] - 2026-08-28 · 抢占式多任务与进程调度

**Added**
- 进程模型：PCB（pid、READY/RUNNING/BLOCKED/ZOMBIE/FREE 状态机）、进程表
- 抢占式轮转调度：PIT 100Hz 心跳驱动 `sched_tick`，多进程轮流运行
- 调度原语：`yield`（主动让出）、`sleep`（按 tick 阻塞并按时唤醒）、`exit`/`kill`（退出 + 僵尸回收）
- 内核 idle 进程（PID 0）：无就绪进程时 `sti; hlt` 兜底，负责状态刷新与键盘回显
- 多进程共享同一份用户代码页，各自独立内核栈 + 用户栈
- 宿主单元测试 `tests/test_sched.c`（调度队列策略，84 条断言）

**Engineering**
- 源码/产物分离：`src/` + `build/`；`make clean` 一键清理
- `sched_policy.c` 抽成纯逻辑模块，支持宿主单测
- QEMU 回归新增 idle 心跳、定时器心跳校验项

**Fixed**
- BUG-001（早期遗留确认）：上下文切换改为 `jmp resume_point` 恢复现场
- BUG-002：idle 空队列返回路径不再 `cli; hlt`，修复系统挂死

## [v0.4] - 用户态 ring3 与系统调用

**Added**
- 重建 GDT（kernel/user 代码段 + 数据段 + TSS），`ltr` 加载 TSS
- `int 0x80` 系统调用门（DPL=3）：`exit / print / get_ticks / sleep / yield / get_pid`
- ring3 用户程序加载与运行（共享代码页 0x80000000）
- 内存保护演示：用户态写内核地址 0xB8000 → 页错误 → 进程终止
- 键盘回显主循环

## [v0.3] - 内存管理

**Added**
- 物理页帧分配器（4KB 粒度）
- 分页开启 + 页表管理（`map_page`）
- 内核堆 `kmalloc/kfree`（首适应）
- 懒分配：缺页按需映射，`pf_handler` 支持懒分配区恢复
- 内存自检与状态行（free / heap / lazy）

## [v0.2] - C 内核地基

**Added**
- multiboot 引导（QEMU `-kernel` 直接加载）
- GDT/IDT、8259 PIC 重映射、CPU 异常处理
- PIT 定时器（100Hz 心跳）、PS/2 键盘、VGA 文本 + 串口输出
- 交互式回显（可键入内容，回车换行）

## [v0.1] - 引导与保护模式

**Added**
- 软盘引导扇区（`v1-floppy/`）
- 实模式 → 保护模式切换
- VGA 打印 "Hello Micro-OS!"
