# Bug 记录（Bug Log）

> 记录开发过程中遇到的真实 Bug：现象、根因、修复、回归验证。
> 编号唯一；状态：`已修复`（当前均如此）。新问题直接追加。

---

## BUG-001 [已修复] v0.4 上下文切换导致被抢占进程崩溃

- **版本**：v0.4（引入用户态/系统调用时）
- **现象**：进程被定时器抢占后再次被切回时触发内核页错误，系统崩溃。
- **根因**：`sched_switch_esp` 原先用 `ret` 返回，依赖 `[esp-4]` 存放返回地址。
  但被抢占进程的 `[esp-4]` 是 `push esp` 压入的现场指针（gs 槽），**不是代码地址**，
  进程恢复时把数据当指令跳转，必然崩溃。
- **修复**：
  - `isr.s`：`sched_switch_esp` 改为 `mov esp, 目标` 后**直接 `jmp resume_point`**，不再依赖返回地址。
  - `frame_build`：去掉栈上的返回地址槽位；所有调用点统一传 `kernel_esp`（gs 槽地址），不再 `-4`。
- **回归**：QEMU 回归中"procA/procB 抢占打印"持续通过；多次抢占后进程仍存活。

---

## BUG-002 [已修复] v0.5 idle 进程挂死，定时器中断"消失"

- **版本**：v0.5（新增调度器后）
- **现象**：进程都睡眠后切入 idle，idle 进入 `hlt` 后系统冻结，
  串口不再输出周期心跳（`alive=` / `ticks=`），如同定时器中断停止。
- **排查过程**：为定位加了大量诊断（读 PIC ISR/IRR、PIT 计数器、EFLAGS.IF），
  一度怀疑硬件定时器；最终确认中断其实一直在触发。
- **根因**：`sched_tick`/`sched_yield` 在 `schedule()` 后写了
  `cli; hlt` 并注释"不可达"。但 `schedule()` 有一个**正常返回路径**：
  当前进程是 idle 且就绪队列为空时直接 `return`。
  于是 idle 的 `hlt` 被定时器唤醒后，中断处理一路进到 `sched_tick`，
  执行 `cli; hlt` —— 关中断并停机，从此再也没有任何中断能唤醒 CPU。
- **修复**：该返回路径改为**正常返回**，让 `iret` 回到 idle 循环
  继续刷新状态并再次 `hlt` 等待心跳（见 [src/sched.c](v2-c-kernel/src/sched.c) 中 `sched_tick`/`sched_yield`）。
- **回归**：QEMU 回归中"idle 状态行心跳"与"定时器心跳正常"由失败转通过；
  串口日志可见 100Hz 心跳持续、内存稳定无泄漏。

---

## BUG-003 [已修复] 工程 v0.5 目录重构后用户程序嵌入符号失效

- **版本**：v0.5（工程整理，源码移入 `src/`、产物移入 `build/` 后）
- **现象**：`make` 链接报 `undefined reference to _binary_userprog_bin_start`。
- **根因**：`objcopy -I binary` 生成的符号名来自**输入文件路径**；
  二进制从 `userprog.bin` 移到 `build/userprog.bin` 后，
  符号变成 `_binary_build_userprog_bin_start`，与内核代码引用的旧名不符。
- **修复**：Makefile 中 `cd build && objcopy ... userprog.bin`，以纯文件名转换，
  保持符号名 `_binary_userprog_bin_start` 稳定（见 [Makefile](v2-c-kernel/Makefile)）。
- **回归**：`make clean && make test` 全绿。

---

## BUG-004 [已修复] v0.6 信号量等待者被定时器"误唤醒"

- **版本**：v0.6（引入信号量阻塞后）
- **现象**：`sched_tick` 原唤醒条件只判 `state == PROC_BLOCKED && ticks >= wakeup_tick`。
  若把该逻辑直接用于信号量等待者，其 `wakeup_tick` 未赋值（0），
  `ticks - wakeup_tick >= 0` 恒成立 → 信号量等待者会被定时器**提前误唤醒**，
  进入临界区时锁仍未释放，破坏互斥语义。
- **根因**：PCB 只有"是否阻塞"没有"为何阻塞"；定时唤醒（sleep）与事件唤醒（sem）混用同一判定。
- **修复**：PCB 新增 `block_reason` 字段（`BLOCK_NONE / BLOCK_SLEEP / BLOCK_SEM`）；
  `sched_tick` 只唤醒 `BLOCK_SLEEP` 的进程；信号量等待者一律由 `sched_wake` 显式唤醒。
- **回归**：串口日志中，sem 阻塞进程（如 `[sem] wait pid=3 id=1 -> block`）跨多个 tick
  仍保持阻塞，直到对端 `[sem] signal id=1 -> wake pid=3` 才被唤醒。

---

## BUG-005 [已修复] v0.6 阻塞系统调用唤醒后返回值错误

- **版本**：v0.6（信号量 wait 阻塞路径）
- **现象**：进程在 `sem_wait` 上阻塞，被唤醒并 `iret` 恢复用户态后，
  `eax` 仍是系统调用号，`syscall3` 返回垃圾值，可能被当作地址/计数使用。
- **根因**：被阻塞进程的现场是 `int 0x80` 中断时的寄存器快照（`eax=系统调用号`）；
  阻塞路径只保存了现场、没有预写返回值，唤醒恢复后自然不带正确返回值。
- **修复**：`sched_wake` 把被唤醒进程保存帧的 `eax` 置 0；`sys_sem_wait` 阻塞前也预写 `r->eax = 0`，
  双保险保证唤醒后系统调用返回 0（成功）。
- **回归**：互斥共享计数演示中，被唤醒进程能正确继续执行（最终 `cnt=10`，两进程各 +5）。

---

## BUG-006 [已修复] v0.8 文件系统演示与测试的越界/实参个数错误

- **版本**：v0.8（文件系统系统调用 + 演示进程 + 宿主单测开发期）
- **现象**：
  - `userprog.c` 中 `user_fs_l` 调用 `syscall3(SYS_FS_WRITE, 3, (uint32_t)"hello ls!", 9, 0)`
    传入 5 个实参（`syscall3` 只接受 4 个），编译报"实参过多"。
  - `tests/test_fs.c` 跨块写入测试把 10000 字节写进 300 字节的栈缓冲，栈越界；
    且 inode 耗尽断言未计入已占用的 inode（根目录 + x.txt + y.txt 共 3 个），断言值偏大。
- **根因**：手写系统调用参数个数笔误；测试缓冲尺寸与断言基数未随用例扩展同步更新。
- **修复**：
  - `user_fs_l` 删去多余实参：`syscall3(SYS_FS_WRITE, 3, (uint32_t)"hello ls!", 9)`。
  - `test_fs.c` 增加 `char big[600]` 承载 500 字节读回；inode 耗尽断言改为 `FS_MAX_INODES - 3`。
- **回归**：`make test` 全绿（宿主 6 项、QEMU 回归 v0.8 检查项全通过）。

---

## BUG-007 [已修复] v0.9 ELF 首段含文件头页，映射区间不含它导致加载缺页

- **版本**：v0.9（ELF 动态加载器 + shell 应用开发期）
- **现象**：shell ELF 用 `-Ttext 0x80030000` 链接，首个 PT_LOAD 段 vaddr 为 `0x8002f000`
  （ELF 头落在目标地址的**前一页**）。`load_elf_file` 原先按固定 `APP_REGION`（0x80040000）设定
  `load_vbase/load_region`，`app_mapfn` 拒绝映射落在区间外的 `0x8002f000` 页，
  `elf_load` 向未映射地址拷贝时触发页错误。
- **根因**：映射区间硬编码/不与 ELF 自身段对齐；忽略了 `-Ttext` 会把 ELF 头放到前一页这一事实。
- **修复**：
  - `elf.c` 新增 `elf_load_range(data, size, &base, &end)`：仅扫描 PT_LOAD 段，返回页对齐覆盖区间。
  - `load_elf_file` 改用该区间设置 `load_vbase/load_region`，保证 ELF 头页也被映射。
- **回归**：QEMU 回归中"内核加载 shell ELF"、"shell 提示符"由失败转通过；
  `run hello` / `run crash` 端到端加载并退出均正常。

---

## BUG-008 [已修复] v0.10 串口终端测试：Unix socket 通道收不到输出

- **版本**：v0.10（串口接收 + `tests/test_serial.sh` 开发期；属**测试脚手架**问题，非内核缺陷）
- **现象**：先用 Unix socket 做 QEMU 串口通道时，日志文件始终为空，
  仿佛内核没有向串口输出任何内容。
- **根因**：QEMU 的 `-serial unix:` 在**没有客户端连接时会丢弃输出数据**；
  脚本启动 QEMU 后立即开始收集，在客户端（`nc`/`socat`）连上之前，
  内核启动期的输出（含 shell 提示符）已经被丢弃，后续断言自然全部落空。
- **修复**：改用**一对 FIFO 管道**承载串口双向通道：
  - 写方向：脚本先 `exec 9>` 固定 fd 打开 FIFO 写端，QEMU stdin 接该 FIFO，随时可写；
  - 读方向：`cat fifo > 日志` 常驻收集，QEMU stdout 接该 FIFO，全程不丢数据。
  - 最终以纯 bash（固定 fd）实现，避免依赖 Python/ptty，跨环境可移植。
- **回归**：`tests/test_serial.sh` 稳定通过（help/ls/cat/run hello/run echo/run crash 全命中）。

---

## BUG-009 [已修复] v0.11 引导期过早开中断，调度器抢跑导致引导乱序

- **版本**：v0.11（引入每进程地址空间，ELF 加载期间切 CR3 时）
- **现象**：串口日志乱序——`[elf] 'shell' loaded` 之后不是预期的
  `spawn_at pid=10 name=shell`，而是用户进程 `[A] procA started` 立刻开跑；
  `[boot] shell pid=10` 与 `spawn_at pid=10` 被推迟到数百行之后；
  且 `[sched] start -> pid=1` 与 idle 心跳（`alive=`）从未出现。
- **根因**：`usermode_spawn_elf` 为保护 CR3 切换执行 `cli`，切回后却紧跟一条
  `sti`。此时内核仍在引导期（PIT 已初始化、PIC 已放开 IRQ0），`sti` 一执行，
  定时器中断立刻抢占内核——**在 shell 尚未注册、`sched_start()` 尚未执行之前**
  就切入了用户进程 procA。之后内核被中断的 `sched_spawn_at` 现场只能等
  下一轮定时器抢占才恢复，导致 spawn_at/boot shell/sched_start 全部推迟、
  `sched_start` 面对空就绪队列走 idle 分支，日志断言因此失败。
- **修复**：删去 `usermode_spawn_elf` 里 CR3 切回后的 `sti`，保持关中断：
  - 引导路径：由 `sched_start()` 的 iret 恢复 eflags(IF=1) 开启中断；
  - syscall 路径：由 iret 恢复用户 eflags(IF=1)；`sched_spawn_at` 不阻塞，安全。
- **回归**：QEMU 回归中"切入第一个进程"（`start -> pid=1`）与"idle 状态行心跳"（`alive=`）
  由失败转通过；`make test` 全绿。

---

## BUG-010 [已修复] v0.12 exec：释放旧地址空间后才 set_name，name 指向旧用户栈

- **版本**：v0.12（引入 sys_exec 镜像替换时）
- **现象**：exec 流程中若 name 指向旧用户栈，在旧地址空间释放后读它会缺页。
- **根因**：`sched_exec` 先 `release_priv_frames + addr_space_destroy` 释放旧地址空间，
  之后才 `set_name(p, name)`——而 `name` 是调用方从 shell 栈（fork 深拷贝的旧用户栈）
  传入的字符串指针。这是独立于 BUG-011 的隐患（最初 @8001aee8 fault 的真凶是 BUG-011）。
- **修复**：`set_name` 移到 `sched_exec` 开头（释放任何用户资源之前），防御性正确。
- **回归**：`exec args` 在 QEMU/串口回归通过。

## BUG-011 [已修复] v0.12 exec：切 CR3 后 load_elf_file 才读 name → 缺页（@8001aee8）

- **版本**：v0.12（引入 sys_exec 时）
- **现象**：`exec args` 触发 `[FATAL] page fault @8001aee8 err=0 eip=105eea`，内核停机。
- **根因**：`sys_exec` 为加载 ELF 先把 CR3 切到**新地址空间**（用户半区为空），
  而 `load_elf_file(name)` 里的 `fs_lookup` 要读 `name`（指向旧用户栈 0x8001aee8）。
  CR3 切换后旧栈地址在新地址空间无映射 → 缺页。
  这是 v0.11 `usermode_spawn_elf` 的 namebuf 问题的同类场景，但 sys_exec 漏了拷贝。
- **修复**：`sys_exec` 在切 CR3 前把 `name` 与 argv 内容全部拷入内核缓冲（namebuf/names）。
- **回归**：`exec args` 通过。

## BUG-012 [已修复] v0.12 argv 的 cdecl 栈布局顺序错位

- **版本**：v0.12（实现 argv 布置时）
- **现象**：`exec args alpha beta gamma` 后 args 打印 `argc=2147606511` 或
  `argv[0]='` 后读字符串地址 0x73677261（"args" 的 ASCII）触发缺页。
- **根因**：`argv_layout` 在用户栈布置 argc/argv 的顺序不符合 cdecl 约定
  （`[esp]=返回地址, [esp+4]=argc, [esp+8]=argv 指针`），多次迭代错位：
  - 先写 fake_ret/argc 再写 argv 数组 → esp 指向数组而非 fake_ret；
  - 缺 argv 指针槽（argv 参数应是"数组地址"，而非直接内联数组首元素）。
- **修复**：按 cdecl 从高到低布置：字符串区 → argv 指针数组 → argv 指针槽(esp+8)
  → argc 槽(esp+4) → fake_ret(esp)。
- **回归**：args 打印 `argc=4` 与 `argv[0..3]` 全对。

## BUG-013 [已修复] v0.13 守卫页布局期判定偏移计算错误

- **版本**：v0.13（用户栈守卫页开发期）
- **现象**：初版守卫页判定的宿主单测边界断言失败——栈区外地址误命中、
  槽内栈页地址误判为守卫页。
- **根因**：布局参数迭代过程中的槽对齐/偏移取值错误：曾用"每进程 4KB 槽 + 独立偏移"，
  与实际映射（栈页不映射守卫页）不一致；且"槽内低半页=守卫"这一约定在早期常量
  （`SLOT`/`GUARD` 取值不匹配）下计算 `(fault & (SLOT-1)) < GUARD` 结果错位。
- **修复**：把布局收敛为 **8KB 槽 = 4KB 守卫 + 4KB 栈页**（`USER_STACK_SLOT=0x2000`、
  `USER_STACK_GUARD=0x1000`），`stack_guard_hit` 只依赖这两个常量；
  `tests/test_guard.c` 用显式地址（pid0/1/2 槽内守卫/栈页边界、跨槽边界、栈区外）
  逐条验证后再跑通。
- **回归**：`make test` 全绿（test_guard 15 条断言 + QEMU/串口回归 stackovf 用例）。

## BUG-014 [已修复] v0.14 sys_wait 的 spawn 后、wait 前竞态：wait 返回 -1

- **版本**：v0.12（引入 sys_wait/alloc_pid 后遗留），v0.14 修复
- **现象**：shell `run hello` / `run isol` / `run forkdemo` / `exec args` 偶发打印
  `exited code=4294967295`（-1）而非真实退出码 0；随负载增大（v0.14 加入 fsdemo）复现变频繁。
- **根因**：`sched_tick` 心跳**无条件回收所有僵尸进程**。shell 的 `sys_spawn_file` 之后
  紧接着 `sys_wait`，但两次系统调用之间可能发生两次定时器抢占：子进程被调度运行并退出
  （置 ZOMBIE）→ 下一次心跳把它**回收为 FREE** → shell 的 `sys_wait` 才执行，
  `sched_get(pid)` 看到 `PROC_FREE` 直接返回 -1（"已回收，退出码丢失"）。
- **修复**：**僵尸延迟回收**——PCB 增加 `parent_pid`：
  - `sched_tick` 只回收"没有父进程会 wait"的僵尸：`parent_pid==0`（boot 演示/孤儿）
    或父进程已 FREE；父进程存活时**保留僵尸**。
  - `sys_wait` 发现子进程 ZOMBIE 时 `sched_reap(pid)` 回收资源并返回其退出码；
  - `terminate_current` 唤醒等待中的父进程后把该子进程置 `parent_pid=0`（退出码已交付，
    僵尸交心跳回收），避免"父进程已唤醒、无人再 reap"的泄漏。
- **回归**：QEMU 回归交互命令恢复严格断言 `exited code=0`，连续 8 次复跑全绿。

## BUG-015 [已修复] v0.14 fs_walk 失败路径未写 leaf/dirout，调用方读未初始化栈值

- **版本**：v0.14（引入路径解析器时）
- **现象**：`fs_mkdir("/none/x")`（父目录不存在）意外返回成功并创建了一个 inode。
- **根因**：`fs_walk` 在"中间组件不存在 / 中间组件非目录 / 层级过深"三种失败路径上
  **直接 `return -1` 而未写 `*leaf`/`*dirout`**；调用方 `fs_make` 读到栈上未初始化的
  `leaf[0]`（非零）误判为"叶子缺失可创建"，用垃圾 `dir`/`leaf` 执行 `dir_add`，
  可能污染目录结构（本应返回 -1 的调用返回了成功）。
- **修复**：`fs_walk` 的所有失败路径统一先写 `leaf[0]=0` 与 `*dirout=dir` 再返回 -1，
  使"叶子缺失"（leaf 非空）与"非法路径"（leaf 空）可区分；`fs_lookup` 直接返回 walk 结果，
  `fs_make`/`fs_list` 依据 leaf 是否为空判断。
- **回归**：test_fs 新增 `/none/x` 等非法路径断言（8686 条全绿）。

## BUG-016 [已修复] v0.15 fsdemo 无 sys_exit → 栈顶 ret 崩溃被误报 STACK OVERFLOW

- **版本**：v0.14（fsdemo 引入时），v0.15 修复
- **现象**：`run fsdemo` 正常完成所有工作后异常终止，退出码应为 0 实为 -1，且打印误导性的
  `[user] STACK OVERFLOW pid=.. @.. -> killed`；shell 报 `exited code=4294967295`。
- **根因**：所有其他应用都在 `app_main` 末尾显式 `sys_exit`，唯独 fsdemo 没有 →
  `app_main` 返回 → `ret` 弹出**栈槽顶端的字**（初始 esp=user_esp_top=槽顶，该字未映射）→
  页错误 → `stack_guard_hit` 旧实现只按 `(fault & 0x1FFF) < 0x1000` 对齐模式判定，
  **不校验地址属于本进程守卫页** → 槽顶边界恰好命中低半页模式 → 误报 STACK OVERFLOW 并 kill。
- **为何测试没抓到**：`tests/test_serial.sh` 只 grep `[fsdemo] done`，没有像 hello/isol/forkdemo
  那样断言 `exited code=0`（回归盲区的典型案例）。
- **修复（双管齐下）**：
  1. **CRT 收口**（v0.16）：ELF 入口改 `_start`，`app_main` 返回后统一 `sys_exit(0)`，
     根除"忘写 sys_exit 从栈顶 ret"整类问题；
  2. **guard.c 改为按 pid 判定**：`stack_guard_hit(fault, pid)` 只认定 fault 落在
     `[BASE+pid*SLOT, +GUARD)`（本进程守卫页）才是栈溢出，槽顶边界归下一槽，不再误报。
- **回归**：test_guard 新增槽顶边界/跨槽归属断言（22 条）；serial/qemu 补 fsdemo 退出码断言；
  `make test` 全绿。
- **教训**：关键字断言验证不了"退出码"这类不变量 → 引入 shell `selftest` 单行结构化自检
  与各应用退出码断言（见 design.md §10）。

## BUG-017 [已修复] v0.16 引入 CRT 后 spawn 路径入口读 argc/argv 越出栈页

- **版本**：v0.16（把 ELF 入口从 `app_main` 改为 `_start` 时引入）
- **现象**：`run hello` 等经 spawn 启动的应用一切正常，但**常驻 shell（spawn 路径）**
  一启动就页错误：`[user] PAGE FAULT pid=10 @80026008 err=4 -> killed`，
  串口无提示符、交互回归大面积失败。
- **根因**：spawn 路径 `frame_build` 把 `user_esp` 设为 `user_esp_top`（栈页**顶**），
  栈页只映射到 `[stk, 槽顶)`，槽顶上方未映射。旧入口 `app_main` 忽略 argc/argv 时
  从不读 `[esp+8]`（编译器连引用都没有）故侥幸可用；新入口 `_start` **必然**读
  `[esp+8]`/`[esp+12]` 取 argv/argc 转给 `app_main` → 读取未映射地址 → 页错误。
- **修复**（sched.c `entry_block`）：spawn 路径把入口 cdecl 块 `[fake_ret][argc][argv]`
  写在**栈页顶下方 12B**（esp = 槽顶-12），三槽全在已映射栈页内；无参启动用 argc=0/argv=0。
  exec 路径本就由 `argv_layout` 布置真实 argv，不受影响。
- **回归**：serial/qemu 回归 shell 提示符、`run hello`、`exec args` argv 校验全部通过。
- **教训**：更换入口约定时，必须核对"新入口会读哪些栈上参数、这些地址是否已映射"。

## BUG-018 [已修复] v0.18 e1000 TX 轮询被编译器优化掉（描述符环非 volatile）

- **版本**：v0.18（e1000 驱动开发期）
- **现象**：`e1000_tx` 填好描述符、写 TDT 后，轮询描述符 `status.DD` 3M 次总超时
  返回 -1；pcap（filter-dump）只有 24 字节头部、**没有任何包发出**。
- **排查**：反汇编发现等待循环被优化成**单次判断**（`testb $0x1; jne` 后直接 ret）——
  编译器把 `d->status` 的读取当循环不变量提升到循环外。因为 `tx_ring` 是普通数组，
  编译器"看不到"设备会异步改写 status，认为循环内读值不变，直接摊平。
- **根因**：描述符环（设备 DMA 写 status/DD 位）**必须 volatile**，否则 GCC -O2 会把
  轮询读提升/缓存，等待循环形同虚设。
- **修复**：
  1. `rx_ring/tx_ring` 声明为 `volatile`；
  2. **关键**：局部指针也要带 volatile——`struct e1000_tx_desc *d = &tx_ring[idx]`
     取地址会**丢弃 volatile 限定符**（`volatile struct*` → `struct*`），`d->status`
     又变回普通读，循环再次被优化。改为 `volatile struct e1000_tx_desc *d` 后才真正恢复。
- **回归**：pcap 出现完整 ARP 请求/回复；`make test-net` 通过。
- **教训**：轮询 DMA 完成位时，**数组和取地址后的指针都要 volatile**；用 objdump
  核对关键轮询循环是否真的在循环体内重读内存。

---

## BUG-019 [已修复] v0.18 e1000 TCTL/RCTL 使能位写错（EN 是 bit1 不是 bit0）

- **版本**：v0.18（e1000 驱动开发期）
- **现象**：volatile 修复后 TX 仍超时：`TDH=0 TDT=1 TCTL=9 TPT=0`——QEMU 收到了 TDT=1
  （读回正确）但**从未处理描述符**（TDH 不动、TPT 不增），pcap 依旧无包。
- **根因**：驱动里 `TCTL_EN = 1u<<0`、`RCTL_EN = 1u<<0`，但 **e1000 的使能位是 bit1**：
  - QEMU 定义 `E1000_TCTL_EN = 0x00000002`、`E1000_RCTL_EN = 0x00000002`
  - 我们写出的 `TCTL = EN(0x1)|PSP(0x8) = 0x9`，bit1=0 → QEMU `start_xmit` 第一行
    `if (!(TCTL & EN)) return;` 判定 **TX 未使能**，直接返回，TDH 恒 0。
- **修复**：`TCTL_EN = 1u<<1`、`RCTL_EN = 1u<<1`（对齐 Intel 手册与 QEMU 定义）。
- **回归**：ARP 请求发出、SLIRP 回复收到，自检通过；`make test-net` 全绿。
- **教训**：寄存器使能位务必对照手册/模拟器定义，不要凭"通常从 bit0 起"猜测；
  TDH 不动 + TPT=0 是"设备根本没使能"的典型信号。

---

## BUG-020 [已修复] v0.18 QEMU 特例：RCTL 触发 1000ms 收包排队窗口

- **版本**：v0.18（e1000 驱动开发期；**QEMU 模拟器行为**，非真实硬件缺陷）
- **现象**：TX 打通后自检仍要**重发 4~5 次**才收到 ARP 回复；pcap 显示 5 个请求都有回复，
  但驱动前 4 次轮询期间 RX 环里**一个包都没有**。
- **根因**：QEMU e1000 的 `set_rx_control`（写 RCTL）会
  `timer_mod(flush_queue_timer, now+1000ms)`；而 `e1000_can_receive` 与
  `e1000_receive_iov` 都检查 `!timer_pending(flush_queue_timer)`——**写 RCTL 后 1000ms 内
  收到的包被排队、不进 RX 环**，直到 flush 定时器到期才批量写入。驱动自检恰好在
  使能 RX 后立即收发，头 1 秒自然什么都收不到。
- **修复**：`e1000_init` 末尾等 flush 窗口过期（300M 次 volatile 空转）再返回；
  自检重试循环（5 次）仍保留兜底。
- **回归**：自检**一次**通过（`selftest: rx ARP reply` 出现在 attempt=0）；`make test-net` 全绿。
- **教训**：模拟器行为可能与真机不同（真实 82540EM 无此 1 秒排队窗口）；
  跨模拟器/真机调试时，先确认"包是否真的进了驱动可见的队列"，再怀疑驱动收发逻辑。

---

## BUG-021 [已修复] v0.25 DHCP 接口签名三处不一致（缺 router 出参）

- **版本**：v0.25（新增 `src/net/dhcp.c/h` + `tests/test_dhcp.c` 开发期）
- **现象**：
  1. `make` 内核编译报 `conflicting types for 'dhcp_parse_reply'`：头文件声明 8 参
     （含 `router` 出参），`dhcp.c` 实现只有 7 参（缺 `router`）；
  2. `test_dhcp.c` 正/负路径调用只传 7 个实参（正路径漏 `&rt`、负路径漏一个 `0`），
     且 `mt` 声明为 `uint32_t` 与 `uint8_t *` 出参不匹配，编译报错/警告；
  3. 修复 1/2 后 `build_reply`（测试手工构造的 DHCP 应答）**未写 option 3（router）**，
     新增的 `CHECK_EQ(rt, 0x0A000202u)` 必然失败。
- **根因**：接口签名演进（为取网关新增 `router` 出参）时，**头文件/实现/测试三处没有同步落地**：
  只改了声明，实现仍按旧 7 参，测试更是在旧签名上编写；
  测试构造的应答又与"解析器应提取 option 3"这一新契约脱节。
- **修复**：三处对齐为 8 参（`dhcp_parse_reply(…, router, lease)`）；
  `build_reply` 补写 option 3；`mt` 改 `uint8_t`。
- **回归**：宿主单测 14/14（test_dhcp 38 断言，含网关提取与全部拒绝路径）；
  QEMU 网络回归 DHCP 四项断言全绿（OFFER/ACK 真实带 router）。
- **教训**：跨文件接口变更要"一处声明、多处实现"同步落地；C 的 `conflicting types`
  与单测是把"实现/测试与声明不一致"兜出来的第一道防线——尽早编译、尽早跑单测，
  比写完再统一检查成本低得多。

---

## BUG-022 [已修复] v0.25 纯逻辑模块误用 `<string.h>`：宿主单测能编、内核 freestanding 编不过

- **版本**：v0.25（`src/net/dhcp.c` 引入时）
- **现象**：宿主单测（test_dhcp）编译运行全绿；但 `make`（内核）编译 dhcp.c 报
  `fatal error: bits/libc-header-start.h: No such file or directory`，内核链接失败。
- **根因**：内核以 `-ffreestanding -nostdlib` 编译，**没有 libc 头文件路径**；
  `dhcp.c` 里 `#include <string.h>` 用了 `memset`。宿主单测走系统 gcc（默认带 glibc），
  故宿主能编；内核链路无 glibc，`<string.h>` 展开时找不到 `bits/libc-header-start.h`。
- **修复**：`dhcp.c` 去掉 `#include <string.h>`，`memset(bootp,0,240)` 改手写清零循环。
- **回归**：`make` 内核构建成功；宿主单测 14/14 不受影响（test_dhcp.c 本身是宿主测试，仍可合法用 libc）。
- **教训**：可宿主单测的纯逻辑模块**必须同时能在 freestanding 内核下编译**——
  宿主测试通过 ≠ 内核可编译。`run_host_tests.sh` 只跑宿主 gcc 链路，内核编译需单独 `make`；
  纯逻辑源文件里禁用 libc 头（`memset/strlen/memcpy` 等），自备小工具或手写循环。

---

## BUG-023 [已修复] v0.21 selftest 检查项 5→6 升级时，多脚本断言漏同步

- **版本**：v0.21（selftest 新增第 6 项"内核自审计"，`PASS (5 checks)` → `PASS (6 checks)`）
- **现象**：qemu_regression.sh 已更新为 `PASS (6 checks)`，但 `tests/test_serial.sh` 与
  `tests/test_persist.sh` 仍断言 `PASS (5 checks)`，串口/持久化回归失败。
- **根因**：同一句"魔法断言字符串"散落在**多个回归脚本**里（qemu/serial/persist 各一份），
  升级检查项时只改了其中一处，另两处漏改——升级与断言修改不是原子的。
- **修复**：test_serial.sh / test_persist.sh 同步为 `PASS (6 checks)`。
- **回归**：`make test` 全绿（host + qemu + serial + persist）。
- **教训**：跨脚本重复的断言字符串是"升级易漏改"的经典盲区；此类共享期望值
  （如 selftest 的 `N checks`）应抽成单一来源（共享 shell 变量/文件），或至少用
  `grep -q "PASS ([0-9]* checks)"` 之类不绑定具体数字的宽松断言。

---

## BUG-024 [已修复] v0.26 deep 演示程序尾递归被 -O2 改写为循环，栈不生长

- **版本**：v0.26（用户栈按需生长，新增 `src/app/deep.c` 演示程序）
- **现象**：`deep` 程序递归 12 层、每层声明 `char buf[1024]`，期望 ESP 逐页下探触发
  3 次 `[stack] grow`；实际运行 0 次生长直接打印存活，QEMU/串口回归断言失败。
- **根因**：应用统一以 `-O2` 编译。`deep` 是**尾递归**（最后一句即 `deep(n-1)`），
  GCC 优化将其改写为循环、栈帧被复用，栈占用恒 < 4KB，永远碰不到守卫页——
  代码"没跑错"，是优化器把演示对象优化没了。
- **修复**：`deep` 函数加 `__attribute__((noinline, optimize("O0")))` 强制关优化，
  `buf` 声明为 `volatile` 并真实写入（`buf[0]=(char)n`），确保每层帧必在栈上分配；
  每层 1KB × 12 = 12KB > 初始 4KB → 命中守卫页 3 次生长后存活。
- **回归**：`make test` 全绿（宿主 34 断言 + QEMU/串口 `deep` 用例：启动 → `[stack] grow`
  ×3 → 存活 → 退出码 0）。
- **教训**：写"压栈/递归"类演示程序时，编译优化可能把要演示的现象优化掉——
  要么关闭该函数优化，要么让每帧有真实内存访问（volatile 写），并在回归里断言
  "现象确实发生"（如 `[stack] grow` 日志），而不是只断言"程序没崩"。

---

## 工程踩坑（非代码缺陷）

| 编号 | 场景 | 现象 | 处置/教训 |
|------|------|------|-----------|
| OPS-001 | git 提交（沙箱环境） | `Author identity unknown`：环境未配置 user.name/user.email | 逐次用 `git -c user.name=… -c user.email=… commit` 指定，不改全局 config；`git log` 历史可溯源 |
| OPS-002 | 编辑 selftest 日志调用 | 改 ARP 自检日志时把 `serial_printf("… #%d", attempt)` 误改为 `serial_puts("… #%d")` | `serial_puts` 单参、`%d` 只是普通字符 → **编译不报错、输出丢参数**；且现有断言 `selftest: tx ARP req` 不校验 `#N` 数字 → 回归抓不到。提交前人工核对日志格式参数；日志格式化参数丢失类问题应靠"打印变量值"的断言或 diff 日志发现 |

## 未解决问题（观察记录）

| 编号 | 现象 | 结论 |
|------|------|------|
| OBS-001 | 定时器中断内读 PIT 计数器常为 0 | 正常现象：中断发生在计数器回绕时，读到的瞬时值即 0，并非硬件故障 |
