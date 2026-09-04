# Bug 记录（Bug Log）

> 记录开发过程中遇到的真实 Bug：现象、根因、修复、回归验证。
> 编号唯一；状态：`已修复` 或 `待修复`（已核实、带复现、待排期）。新问题直接追加。

***

## BUG-001 \[已修复] v0.4 上下文切换导致被抢占进程崩溃

* **版本**：v0.4（引入用户态/系统调用时）

* **现象**：进程被定时器抢占后再次被切回时触发内核页错误，系统崩溃。

* **根因**：`sched_switch_esp` 原先用 `ret` 返回，依赖 `[esp-4]` 存放返回地址。
  但被抢占进程的 `[esp-4]` 是 `push esp` 压入的现场指针（gs 槽），**不是代码地址**，
  进程恢复时把数据当指令跳转，必然崩溃。

* **修复**：

  * `isr.s`：`sched_switch_esp` 改为 `mov esp, 目标` 后**直接** **`jmp resume_point`**，不再依赖返回地址。

  * `frame_build`：去掉栈上的返回地址槽位；所有调用点统一传 `kernel_esp`（gs 槽地址），不再 `-4`。

* **回归**：QEMU 回归中"procA/procB 抢占打印"持续通过；多次抢占后进程仍存活。

***

## BUG-002 \[已修复] v0.5 idle 进程挂死，定时器中断"消失"

* **版本**：v0.5（新增调度器后）

* **现象**：进程都睡眠后切入 idle，idle 进入 `hlt` 后系统冻结，
  串口不再输出周期心跳（`alive=` / `ticks=`），如同定时器中断停止。

* **排查过程**：为定位加了大量诊断（读 PIC ISR/IRR、PIT 计数器、EFLAGS.IF），
  一度怀疑硬件定时器；最终确认中断其实一直在触发。

* **根因**：`sched_tick`/`sched_yield` 在 `schedule()` 后写了
  `cli; hlt` 并注释"不可达"。但 `schedule()` 有一个**正常返回路径**：
  当前进程是 idle 且就绪队列为空时直接 `return`。
  于是 idle 的 `hlt` 被定时器唤醒后，中断处理一路进到 `sched_tick`，
  执行 `cli; hlt` —— 关中断并停机，从此再也没有任何中断能唤醒 CPU。

* **修复**：该返回路径改为**正常返回**，让 `iret` 回到 idle 循环
  继续刷新状态并再次 `hlt` 等待心跳（见 [src/kernel/sched.c](../../v2-c-kernel/src/kernel/sched.c) 中 `sched_tick`/`sched_yield`）。

* **回归**：QEMU 回归中"idle 状态行心跳"与"定时器心跳正常"由失败转通过；
  串口日志可见 100Hz 心跳持续、内存稳定无泄漏。

***

## BUG-003 \[已修复] 工程 v0.5 目录重构后用户程序嵌入符号失效

* **版本**：v0.5（工程整理，源码移入 `src/`、产物移入 `build/` 后）

* **现象**：`make` 链接报 `undefined reference to _binary_userprog_bin_start`。

* **根因**：`objcopy -I binary` 生成的符号名来自**输入文件路径**；
  二进制从 `userprog.bin` 移到 `build/userprog.bin` 后，
  符号变成 `_binary_build_userprog_bin_start`，与内核代码引用的旧名不符。

* **修复**：Makefile 中 `cd build && objcopy ... userprog.bin`，以纯文件名转换，
  保持符号名 `_binary_userprog_bin_start` 稳定（见 [Makefile](../../v2-c-kernel/Makefile)）。

* **回归**：`make clean && make test` 全绿。

***

## BUG-004 \[已修复] v0.6 信号量等待者被定时器"误唤醒"

* **版本**：v0.6（引入信号量阻塞后）

* **现象**：`sched_tick` 原唤醒条件只判 `state == PROC_BLOCKED && ticks >= wakeup_tick`。
  若把该逻辑直接用于信号量等待者，其 `wakeup_tick` 未赋值（0），
  `ticks - wakeup_tick >= 0` 恒成立 → 信号量等待者会被定时器**提前误唤醒**，
  进入临界区时锁仍未释放，破坏互斥语义。

* **根因**：PCB 只有"是否阻塞"没有"为何阻塞"；定时唤醒（sleep）与事件唤醒（sem）混用同一判定。

* **修复**：PCB 新增 `block_reason` 字段（`BLOCK_NONE / BLOCK_SLEEP / BLOCK_SEM`）；
  `sched_tick` 只唤醒 `BLOCK_SLEEP` 的进程；信号量等待者一律由 `sched_wake` 显式唤醒。

* **回归**：串口日志中，sem 阻塞进程（如 `[sem] wait pid=3 id=1 -> block`）跨多个 tick
  仍保持阻塞，直到对端 `[sem] signal id=1 -> wake pid=3` 才被唤醒。

***

## BUG-005 \[已修复] v0.6 阻塞系统调用唤醒后返回值错误

* **版本**：v0.6（信号量 wait 阻塞路径）

* **现象**：进程在 `sem_wait` 上阻塞，被唤醒并 `iret` 恢复用户态后，
  `eax` 仍是系统调用号，`syscall3` 返回垃圾值，可能被当作地址/计数使用。

* **根因**：被阻塞进程的现场是 `int 0x80` 中断时的寄存器快照（`eax=系统调用号`）；
  阻塞路径只保存了现场、没有预写返回值，唤醒恢复后自然不带正确返回值。

* **修复**：`sched_wake` 把被唤醒进程保存帧的 `eax` 置 0；`sys_sem_wait` 阻塞前也预写 `r->eax = 0`，
  双保险保证唤醒后系统调用返回 0（成功）。

* **回归**：互斥共享计数演示中，被唤醒进程能正确继续执行（最终 `cnt=10`，两进程各 +5）。

***

## BUG-006 \[已修复] v0.8 文件系统演示与测试的越界/实参个数错误

* **版本**：v0.8（文件系统系统调用 + 演示进程 + 宿主单测开发期）

* **现象**：

  * `userprog.c` 中 `user_fs_l` 调用 `syscall3(SYS_FS_WRITE, 3, (uint32_t)"hello ls!", 9, 0)`
    传入 5 个实参（`syscall3` 只接受 4 个），编译报"实参过多"。

  * `tests/test_fs.c` 跨块写入测试把 10000 字节写进 300 字节的栈缓冲，栈越界；
    且 inode 耗尽断言未计入已占用的 inode（根目录 + x.txt + y.txt 共 3 个），断言值偏大。

* **根因**：手写系统调用参数个数笔误；测试缓冲尺寸与断言基数未随用例扩展同步更新。

* **修复**：

  * `user_fs_l` 删去多余实参：`syscall3(SYS_FS_WRITE, 3, (uint32_t)"hello ls!", 9)`。

  * `test_fs.c` 增加 `char big[600]` 承载 500 字节读回；inode 耗尽断言改为 `FS_MAX_INODES - 3`。

* **回归**：`make test` 全绿（宿主 6 项、QEMU 回归 v0.8 检查项全通过）。

***

## BUG-007 \[已修复] v0.9 ELF 首段含文件头页，映射区间不含它导致加载缺页

* **版本**：v0.9（ELF 动态加载器 + shell 应用开发期）

* **现象**：shell ELF 用 `-Ttext 0x80030000` 链接，首个 PT\_LOAD 段 vaddr 为 `0x8002f000`
  （ELF 头落在目标地址的**前一页**）。`load_elf_file` 原先按固定 `APP_REGION`（0x80040000）设定
  `load_vbase/load_region`，`app_mapfn` 拒绝映射落在区间外的 `0x8002f000` 页，
  `elf_load` 向未映射地址拷贝时触发页错误。

* **根因**：映射区间硬编码/不与 ELF 自身段对齐；忽略了 `-Ttext` 会把 ELF 头放到前一页这一事实。

* **修复**：

  * `elf.c` 新增 `elf_load_range(data, size, &base, &end)`：仅扫描 PT\_LOAD 段，返回页对齐覆盖区间。

  * `load_elf_file` 改用该区间设置 `load_vbase/load_region`，保证 ELF 头页也被映射。

* **回归**：QEMU 回归中"内核加载 shell ELF"、"shell 提示符"由失败转通过；
  `run hello` / `run crash` 端到端加载并退出均正常。

***

## BUG-008 \[已修复] v0.10 串口终端测试：Unix socket 通道收不到输出

* **版本**：v0.10（串口接收 + `tests/test_serial.sh` 开发期；属**测试脚手架**问题，非内核缺陷）

* **现象**：先用 Unix socket 做 QEMU 串口通道时，日志文件始终为空，
  仿佛内核没有向串口输出任何内容。

* **根因**：QEMU 的 `-serial unix:` 在**没有客户端连接时会丢弃输出数据**；
  脚本启动 QEMU 后立即开始收集，在客户端（`nc`/`socat`）连上之前，
  内核启动期的输出（含 shell 提示符）已经被丢弃，后续断言自然全部落空。

* **修复**：改用**一对 FIFO 管道**承载串口双向通道：

  * 写方向：脚本先 `exec 9>` 固定 fd 打开 FIFO 写端，QEMU stdin 接该 FIFO，随时可写；

  * 读方向：`cat fifo > 日志` 常驻收集，QEMU stdout 接该 FIFO，全程不丢数据。

  * 最终以纯 bash（固定 fd）实现，避免依赖 Python/ptty，跨环境可移植。

* **回归**：`tests/test_serial.sh` 稳定通过（help/ls/cat/run hello/run echo/run crash 全命中）。

***

## BUG-009 \[已修复] v0.11 引导期过早开中断，调度器抢跑导致引导乱序

* **版本**：v0.11（引入每进程地址空间，ELF 加载期间切 CR3 时）

* **现象**：串口日志乱序——`[elf] 'shell' loaded` 之后不是预期的
  `spawn_at pid=10 name=shell`，而是用户进程 `[A] procA started` 立刻开跑；
  `[boot] shell pid=10` 与 `spawn_at pid=10` 被推迟到数百行之后；
  且 `[sched] start -> pid=1` 与 idle 心跳（`alive=`）从未出现。

* **根因**：`usermode_spawn_elf` 为保护 CR3 切换执行 `cli`，切回后却紧跟一条
  `sti`。此时内核仍在引导期（PIT 已初始化、PIC 已放开 IRQ0），`sti` 一执行，
  定时器中断立刻抢占内核——**在 shell 尚未注册、`sched_start()`** **尚未执行之前**
  就切入了用户进程 procA。之后内核被中断的 `sched_spawn_at` 现场只能等
  下一轮定时器抢占才恢复，导致 spawn\_at/boot shell/sched\_start 全部推迟、
  `sched_start` 面对空就绪队列走 idle 分支，日志断言因此失败。

* **修复**：删去 `usermode_spawn_elf` 里 CR3 切回后的 `sti`，保持关中断：

  * 引导路径：由 `sched_start()` 的 iret 恢复 eflags(IF=1) 开启中断；

  * syscall 路径：由 iret 恢复用户 eflags(IF=1)；`sched_spawn_at` 不阻塞，安全。

* **回归**：QEMU 回归中"切入第一个进程"（`start -> pid=1`）与"idle 状态行心跳"（`alive=`）
  由失败转通过；`make test` 全绿。

***

## BUG-010 \[已修复] v0.12 exec：释放旧地址空间后才 set\_name，name 指向旧用户栈

* **版本**：v0.12（引入 sys\_exec 镜像替换时）

* **现象**：exec 流程中若 name 指向旧用户栈，在旧地址空间释放后读它会缺页。

* **根因**：`sched_exec` 先 `release_priv_frames + addr_space_destroy` 释放旧地址空间，
  之后才 `set_name(p, name)`——而 `name` 是调用方从 shell 栈（fork 深拷贝的旧用户栈）
  传入的字符串指针。这是独立于 BUG-011 的隐患（最初 @8001aee8 fault 的真凶是 BUG-011）。

* **修复**：`set_name` 移到 `sched_exec` 开头（释放任何用户资源之前），防御性正确。

* **回归**：`exec args` 在 QEMU/串口回归通过。

## BUG-011 \[已修复] v0.12 exec：切 CR3 后 load\_elf\_file 才读 name → 缺页（@8001aee8）

* **版本**：v0.12（引入 sys\_exec 时）

* **现象**：`exec args` 触发 `[FATAL] page fault @8001aee8 err=0 eip=105eea`，内核停机。

* **根因**：`sys_exec` 为加载 ELF 先把 CR3 切到**新地址空间**（用户半区为空），
  而 `load_elf_file(name)` 里的 `fs_lookup` 要读 `name`（指向旧用户栈 0x8001aee8）。
  CR3 切换后旧栈地址在新地址空间无映射 → 缺页。
  这是 v0.11 `usermode_spawn_elf` 的 namebuf 问题的同类场景，但 sys\_exec 漏了拷贝。

* **修复**：`sys_exec` 在切 CR3 前把 `name` 与 argv 内容全部拷入内核缓冲（namebuf/names）。

* **回归**：`exec args` 通过。

## BUG-012 \[已修复] v0.12 argv 的 cdecl 栈布局顺序错位

* **版本**：v0.12（实现 argv 布置时）

* **现象**：`exec args alpha beta gamma` 后 args 打印 `argc=2147606511` 或
  `argv[0]='` 后读字符串地址 0x73677261（"args" 的 ASCII）触发缺页。

* **根因**：`argv_layout` 在用户栈布置 argc/argv 的顺序不符合 cdecl 约定
  （`[esp]=返回地址, [esp+4]=argc, [esp+8]=argv 指针`），多次迭代错位：

  * 先写 fake\_ret/argc 再写 argv 数组 → esp 指向数组而非 fake\_ret；

  * 缺 argv 指针槽（argv 参数应是"数组地址"，而非直接内联数组首元素）。

* **修复**：按 cdecl 从高到低布置：字符串区 → argv 指针数组 → argv 指针槽(esp+8)
  → argc 槽(esp+4) → fake\_ret(esp)。

* **回归**：args 打印 `argc=4` 与 `argv[0..3]` 全对。

## BUG-013 \[已修复] v0.13 守卫页布局期判定偏移计算错误

* **版本**：v0.13（用户栈守卫页开发期）

* **现象**：初版守卫页判定的宿主单测边界断言失败——栈区外地址误命中、
  槽内栈页地址误判为守卫页。

* **根因**：布局参数迭代过程中的槽对齐/偏移取值错误：曾用"每进程 4KB 槽 + 独立偏移"，
  与实际映射（栈页不映射守卫页）不一致；且"槽内低半页=守卫"这一约定在早期常量
  （`SLOT`/`GUARD` 取值不匹配）下计算 `(fault & (SLOT-1)) < GUARD` 结果错位。

* **修复**：把布局收敛为 **8KB 槽 = 4KB 守卫 + 4KB 栈页**（`USER_STACK_SLOT=0x2000`、
  `USER_STACK_GUARD=0x1000`），`stack_guard_hit` 只依赖这两个常量；
  `tests/test_guard.c` 用显式地址（pid0/1/2 槽内守卫/栈页边界、跨槽边界、栈区外）
  逐条验证后再跑通。

* **回归**：`make test` 全绿（test\_guard 15 条断言 + QEMU/串口回归 stackovf 用例）。

## BUG-014 \[已修复] v0.14 sys\_wait 的 spawn 后、wait 前竞态：wait 返回 -1

* **版本**：v0.12（引入 sys\_wait/alloc\_pid 后遗留），v0.14 修复

* **现象**：shell `run hello` / `run isol` / `run forkdemo` / `exec args` 偶发打印
  `exited code=4294967295`（-1）而非真实退出码 0；随负载增大（v0.14 加入 fsdemo）复现变频繁。

* **根因**：`sched_tick` 心跳**无条件回收所有僵尸进程**。shell 的 `sys_spawn_file` 之后
  紧接着 `sys_wait`，但两次系统调用之间可能发生两次定时器抢占：子进程被调度运行并退出
  （置 ZOMBIE）→ 下一次心跳把它**回收为 FREE** → shell 的 `sys_wait` 才执行，
  `sched_get(pid)` 看到 `PROC_FREE` 直接返回 -1（"已回收，退出码丢失"）。

* **修复**：**僵尸延迟回收**——PCB 增加 `parent_pid`：

  * `sched_tick` 只回收"没有父进程会 wait"的僵尸：`parent_pid==0`（boot 演示/孤儿）
    或父进程已 FREE；父进程存活时**保留僵尸**。

  * `sys_wait` 发现子进程 ZOMBIE 时 `sched_reap(pid)` 回收资源并返回其退出码；

  * `terminate_current` 唤醒等待中的父进程后把该子进程置 `parent_pid=0`（退出码已交付，
    僵尸交心跳回收），避免"父进程已唤醒、无人再 reap"的泄漏。

* **回归**：QEMU 回归交互命令恢复严格断言 `exited code=0`，连续 8 次复跑全绿。

## BUG-015 \[已修复] v0.14 fs\_walk 失败路径未写 leaf/dirout，调用方读未初始化栈值

* **版本**：v0.14（引入路径解析器时）

* **现象**：`fs_mkdir("/none/x")`（父目录不存在）意外返回成功并创建了一个 inode。

* **根因**：`fs_walk` 在"中间组件不存在 / 中间组件非目录 / 层级过深"三种失败路径上
  **直接** **`return -1`** **而未写** **`*leaf`/`*dirout`**；调用方 `fs_make` 读到栈上未初始化的
  `leaf[0]`（非零）误判为"叶子缺失可创建"，用垃圾 `dir`/`leaf` 执行 `dir_add`，
  可能污染目录结构（本应返回 -1 的调用返回了成功）。

* **修复**：`fs_walk` 的所有失败路径统一先写 `leaf[0]=0` 与 `*dirout=dir` 再返回 -1，
  使"叶子缺失"（leaf 非空）与"非法路径"（leaf 空）可区分；`fs_lookup` 直接返回 walk 结果，
  `fs_make`/`fs_list` 依据 leaf 是否为空判断。

* **回归**：test\_fs 新增 `/none/x` 等非法路径断言（8686 条全绿）。

## BUG-016 \[已修复] v0.15 fsdemo 无 sys\_exit → 栈顶 ret 崩溃被误报 STACK OVERFLOW

* **版本**：v0.14（fsdemo 引入时），v0.15 修复

* **现象**：`run fsdemo` 正常完成所有工作后异常终止，退出码应为 0 实为 -1，且打印误导性的
  `[user] STACK OVERFLOW pid=.. @.. -> killed`；shell 报 `exited code=4294967295`。

* **根因**：所有其他应用都在 `app_main` 末尾显式 `sys_exit`，唯独 fsdemo 没有 →
  `app_main` 返回 → `ret` 弹出**栈槽顶端的字**（初始 esp=user\_esp\_top=槽顶，该字未映射）→
  页错误 → `stack_guard_hit` 旧实现只按 `(fault & 0x1FFF) < 0x1000` 对齐模式判定，
  **不校验地址属于本进程守卫页** → 槽顶边界恰好命中低半页模式 → 误报 STACK OVERFLOW 并 kill。

* **为何测试没抓到**：`tests/test_serial.sh` 只 grep `[fsdemo] done`，没有像 hello/isol/forkdemo
  那样断言 `exited code=0`（回归盲区的典型案例）。

* **修复（双管齐下）**：

  1. **CRT 收口**（v0.16）：ELF 入口改 `_start`，`app_main` 返回后统一 `sys_exit(0)`，
     根除"忘写 sys\_exit 从栈顶 ret"整类问题；
  2. **guard.c 改为按 pid 判定**：`stack_guard_hit(fault, pid)` 只认定 fault 落在
     `[BASE+pid*SLOT, +GUARD)`（本进程守卫页）才是栈溢出，槽顶边界归下一槽，不再误报。

* **回归**：test\_guard 新增槽顶边界/跨槽归属断言（22 条）；serial/qemu 补 fsdemo 退出码断言；
  `make test` 全绿。

* **教训**：关键字断言验证不了"退出码"这类不变量 → 引入 shell `selftest` 单行结构化自检
  与各应用退出码断言（见 design.md §10）。

## BUG-017 \[已修复] v0.16 引入 CRT 后 spawn 路径入口读 argc/argv 越出栈页

* **版本**：v0.16（把 ELF 入口从 `app_main` 改为 `_start` 时引入）

* **现象**：`run hello` 等经 spawn 启动的应用一切正常，但**常驻 shell（spawn 路径）**
  一启动就页错误：`[user] PAGE FAULT pid=10 @80026008 err=4 -> killed`，
  串口无提示符、交互回归大面积失败。

* **根因**：spawn 路径 `frame_build` 把 `user_esp` 设为 `user_esp_top`（栈页**顶**），
  栈页只映射到 `[stk, 槽顶)`，槽顶上方未映射。旧入口 `app_main` 忽略 argc/argv 时
  从不读 `[esp+8]`（编译器连引用都没有）故侥幸可用；新入口 `_start` **必然**读
  `[esp+8]`/`[esp+12]` 取 argv/argc 转给 `app_main` → 读取未映射地址 → 页错误。

* **修复**（sched.c `entry_block`）：spawn 路径把入口 cdecl 块 `[fake_ret][argc][argv]`
  写在**栈页顶下方 12B**（esp = 槽顶-12），三槽全在已映射栈页内；无参启动用 argc=0/argv=0。
  exec 路径本就由 `argv_layout` 布置真实 argv，不受影响。

* **回归**：serial/qemu 回归 shell 提示符、`run hello`、`exec args` argv 校验全部通过。

* **教训**：更换入口约定时，必须核对"新入口会读哪些栈上参数、这些地址是否已映射"。

## BUG-018 \[已修复] v0.18 e1000 TX 轮询被编译器优化掉（描述符环非 volatile）

* **版本**：v0.18（e1000 驱动开发期）

* **现象**：`e1000_tx` 填好描述符、写 TDT 后，轮询描述符 `status.DD` 3M 次总超时
  返回 -1；pcap（filter-dump）只有 24 字节头部、**没有任何包发出**。

* **排查**：反汇编发现等待循环被优化成**单次判断**（`testb $0x1; jne` 后直接 ret）——
  编译器把 `d->status` 的读取当循环不变量提升到循环外。因为 `tx_ring` 是普通数组，
  编译器"看不到"设备会异步改写 status，认为循环内读值不变，直接摊平。

* **根因**：描述符环（设备 DMA 写 status/DD 位）**必须 volatile**，否则 GCC -O2 会把
  轮询读提升/缓存，等待循环形同虚设。

* **修复**：

  1. `rx_ring/tx_ring` 声明为 `volatile`；
  2. **关键**：局部指针也要带 volatile——`struct e1000_tx_desc *d = &tx_ring[idx]`
     取地址会**丢弃 volatile 限定符**（`volatile struct*` → `struct*`），`d->status`
     又变回普通读，循环再次被优化。改为 `volatile struct e1000_tx_desc *d` 后才真正恢复。

* **回归**：pcap 出现完整 ARP 请求/回复；`make test-net` 通过。

* **教训**：轮询 DMA 完成位时，**数组和取地址后的指针都要 volatile**；用 objdump
  核对关键轮询循环是否真的在循环体内重读内存。

***

## BUG-019 \[已修复] v0.18 e1000 TCTL/RCTL 使能位写错（EN 是 bit1 不是 bit0）

* **版本**：v0.18（e1000 驱动开发期）

* **现象**：volatile 修复后 TX 仍超时：`TDH=0 TDT=1 TCTL=9 TPT=0`——QEMU 收到了 TDT=1
  （读回正确）但**从未处理描述符**（TDH 不动、TPT 不增），pcap 依旧无包。

* **根因**：驱动里 `TCTL_EN = 1u<<0`、`RCTL_EN = 1u<<0`，但 **e1000 的使能位是 bit1**：

  * QEMU 定义 `E1000_TCTL_EN = 0x00000002`、`E1000_RCTL_EN = 0x00000002`

  * 我们写出的 `TCTL = EN(0x1)|PSP(0x8) = 0x9`，bit1=0 → QEMU `start_xmit` 第一行
    `if (!(TCTL & EN)) return;` 判定 **TX 未使能**，直接返回，TDH 恒 0。

* **修复**：`TCTL_EN = 1u<<1`、`RCTL_EN = 1u<<1`（对齐 Intel 手册与 QEMU 定义）。

* **回归**：ARP 请求发出、SLIRP 回复收到，自检通过；`make test-net` 全绿。

* **教训**：寄存器使能位务必对照手册/模拟器定义，不要凭"通常从 bit0 起"猜测；
  TDH 不动 + TPT=0 是"设备根本没使能"的典型信号。

***

## BUG-020 \[已修复] v0.18 QEMU 特例：RCTL 触发 1000ms 收包排队窗口

* **版本**：v0.18（e1000 驱动开发期；**QEMU 模拟器行为**，非真实硬件缺陷）

* **现象**：TX 打通后自检仍要**重发 4\~5 次**才收到 ARP 回复；pcap 显示 5 个请求都有回复，
  但驱动前 4 次轮询期间 RX 环里**一个包都没有**。

* **根因**：QEMU e1000 的 `set_rx_control`（写 RCTL）会
  `timer_mod(flush_queue_timer, now+1000ms)`；而 `e1000_can_receive` 与
  `e1000_receive_iov` 都检查 `!timer_pending(flush_queue_timer)`——**写 RCTL 后 1000ms 内
  收到的包被排队、不进 RX 环**，直到 flush 定时器到期才批量写入。驱动自检恰好在
  使能 RX 后立即收发，头 1 秒自然什么都收不到。

* **修复**：`e1000_init` 末尾等 flush 窗口过期（300M 次 volatile 空转）再返回；
  自检重试循环（5 次）仍保留兜底。

* **回归**：自检**一次**通过（`selftest: rx ARP reply` 出现在 attempt=0）；`make test-net` 全绿。

* **教训**：模拟器行为可能与真机不同（真实 82540EM 无此 1 秒排队窗口）；
  跨模拟器/真机调试时，先确认"包是否真的进了驱动可见的队列"，再怀疑驱动收发逻辑。

***

## BUG-021 \[已修复] v0.25 DHCP 接口签名三处不一致（缺 router 出参）

* **版本**：v0.25（新增 `src/net/dhcp.c/h` + `tests/test_dhcp.c` 开发期）

* **现象**：

  1. `make` 内核编译报 `conflicting types for 'dhcp_parse_reply'`：头文件声明 8 参
     （含 `router` 出参），`dhcp.c` 实现只有 7 参（缺 `router`）；
  2. `test_dhcp.c` 正/负路径调用只传 7 个实参（正路径漏 `&rt`、负路径漏一个 `0`），
     且 `mt` 声明为 `uint32_t` 与 `uint8_t *` 出参不匹配，编译报错/警告；
  3. 修复 1/2 后 `build_reply`（测试手工构造的 DHCP 应答）**未写 option 3（router）**，
     新增的 `CHECK_EQ(rt, 0x0A000202u)` 必然失败。

* **根因**：接口签名演进（为取网关新增 `router` 出参）时，**头文件/实现/测试三处没有同步落地**：
  只改了声明，实现仍按旧 7 参，测试更是在旧签名上编写；
  测试构造的应答又与"解析器应提取 option 3"这一新契约脱节。

* **修复**：三处对齐为 8 参（`dhcp_parse_reply(…, router, lease)`）；
  `build_reply` 补写 option 3；`mt` 改 `uint8_t`。

* **回归**：宿主单测 14/14（test\_dhcp 38 断言，含网关提取与全部拒绝路径）；
  QEMU 网络回归 DHCP 四项断言全绿（OFFER/ACK 真实带 router）。

* **教训**：跨文件接口变更要"一处声明、多处实现"同步落地；C 的 `conflicting types`
  与单测是把"实现/测试与声明不一致"兜出来的第一道防线——尽早编译、尽早跑单测，
  比写完再统一检查成本低得多。

***

## BUG-022 [已修复] v0.25 纯逻辑模块误用 `<string.h>`：宿主单测能编、内核 freestanding 编不过

* **版本**：v0.25（`src/net/dhcp.c` 引入时）

* **现象**：宿主单测（test\_dhcp）编译运行全绿；但 `make`（内核）编译 dhcp.c 报
  `fatal error: bits/libc-header-start.h: No such file or directory`，内核链接失败。

* **根因**：内核以 `-ffreestanding -nostdlib` 编译，**没有 libc 头文件路径**；
  `dhcp.c` 里 `#include <string.h>` 用了 `memset`。宿主单测走系统 gcc（默认带 glibc），
  故宿主能编；内核链路无 glibc，`<string.h>` 展开时找不到 `bits/libc-header-start.h`。

* **修复**：`dhcp.c` 去掉 `#include <string.h>`，`memset(bootp,0,240)` 改手写清零循环。

* **回归**：`make` 内核构建成功；宿主单测 14/14 不受影响（test\_dhcp.c 本身是宿主测试，仍可合法用 libc）。

* **教训**：可宿主单测的纯逻辑模块**必须同时能在 freestanding 内核下编译**——
  宿主测试通过 ≠ 内核可编译。`run_host_tests.sh` 只跑宿主 gcc 链路，内核编译需单独 `make`；
  纯逻辑源文件里禁用 libc 头（`memset/strlen/memcpy` 等），自备小工具或手写循环。

***

## BUG-023 \[已修复] v0.21 selftest 检查项 5→6 升级时，多脚本断言漏同步

* **版本**：v0.21（selftest 新增第 6 项"内核自审计"，`PASS (5 checks)` → `PASS (6 checks)`）

* **现象**：qemu\_regression.sh 已更新为 `PASS (6 checks)`，但 `tests/test_serial.sh` 与
  `tests/test_persist.sh` 仍断言 `PASS (5 checks)`，串口/持久化回归失败。

* **根因**：同一句"魔法断言字符串"散落在**多个回归脚本**里（qemu/serial/persist 各一份），
  升级检查项时只改了其中一处，另两处漏改——升级与断言修改不是原子的。

* **修复**：test\_serial.sh / test\_persist.sh 同步为 `PASS (6 checks)`。

* **回归**：`make test` 全绿（host + qemu + serial + persist）。

* **教训**：跨脚本重复的断言字符串是"升级易漏改"的经典盲区；此类共享期望值
  （如 selftest 的 `N checks`）应抽成单一来源（共享 shell 变量/文件），或至少用
  `grep -q "PASS ([0-9]* checks)"` 之类不绑定具体数字的宽松断言。

***

## BUG-024 \[已修复] v0.26 deep 演示程序尾递归被 -O2 改写为循环，栈不生长

* **版本**：v0.26（用户栈按需生长，新增 `src/app/deep.c` 演示程序）

* **现象**：`deep` 程序递归 12 层、每层声明 `char buf[1024]`，期望 ESP 逐页下探触发
  3 次 `[stack] grow`；实际运行 0 次生长直接打印存活，QEMU/串口回归断言失败。

* **根因**：应用统一以 `-O2` 编译。`deep` 是**尾递归**（最后一句即 `deep(n-1)`），
  GCC 优化将其改写为循环、栈帧被复用，栈占用恒 < 4KB，永远碰不到守卫页——
  代码"没跑错"，是优化器把演示对象优化没了。

* **修复**：`deep` 函数加 `__attribute__((noinline, optimize("O0")))` 强制关优化，
  `buf` 声明为 `volatile` 并真实写入（`buf[0]=(char)n`），确保每层帧必在栈上分配；
  每层 1KB × 12 = 12KB > 初始 4KB → 命中守卫页 3 次生长后存活。

* **回归**：`make test` 全绿（宿主 34 断言 + QEMU/串口 `deep` 用例：启动 → `[stack] grow`
  ×3 → 存活 → 退出码 0）。

* **教训**：写"压栈/递归"类演示程序时，编译优化可能把要演示的现象优化掉——
  要么关闭该函数优化，要么让每帧有真实内存访问（volatile 写），并在回归里断言
  "现象确实发生"（如 `[stack] grow` 日志），而不是只断言"程序没崩"。

***

## BUG-025 \[已修复] v0.27 sys\_brk 只映射步进页，brk 落页中部时顶部半页未映射

* **版本**：v0.26#2 引入（v0.27 移植 cc500 编译器时暴露）

* **现象**：`cc500` 编译器（任意尺寸 malloc，如 32/1040 字节）运行时被
  `PAGE FAULT pid=3 @801ad000 err=6 -> killed` 终止；`[heap] brk pid=3 801acb10 -> 801ad342 pages=10`
  之后写 `0x801ad000` 缺页。`heapdemo`（页对齐 sbrk）从未触发。

* **根因**：`sys_brk` 扩展映射循环 `uint32_t v = old; while (v < a) { …; v += 0x1000; }`
  从**非页对齐的** **`old`** 起步进。当 `brk` 落在页中部（如 `a=0x801ad342`）时，循环只
  覆盖到 `0x801acb10`（页 `0x801ac000` 内），`v` 下一次跳到 `0x801adb10 >= a` 即停——
  **顶部半页** **`0x801ad000-0x801ad342`** **从未映射**，但 `brk` 语义要求 `[heap_base, brk)` 全部可访问。
  `brk_pages_up`（ceil 取整）记账 1 页，实际映射 0 页 → 记账与映射不一致。

* **修复**：改为映射 `[old,a)` 相交的**所有页**——`v = old & 0xFFFFF000u`（下取整）、
  `vend = (a + 0xFFFu) & 0xFFFFF000u`（上取整），`while (v < vend)`；与 `brk_pages_up`
  记账对齐（usermode.c `case 35`）。

* **回归**：`make test` 全绿；cc500 编译器任意尺寸 malloc 不再缺页，自举闭环通过。

* **教训**：内核的"映射区间"必须按**页相交**处理（下取整起点、上取整终点），不能从
  非对齐地址起步进——页映射与逻辑区间（brk/栈/ELF 段）是两种粒度，混用必出半页空洞。
  另：纯逻辑 `brk_pages_up` 与映射循环是**两处实现**，容易一处改一处不改，应让映射
  循环的取整逻辑与记账函数共用同一公式。

***

## BUG-026 \[已修复] v0.27b cc500 对畸形输入死循环（形参列表 EOF 未闭合）

* **版本**：v0.27（guest 内 cc500 编译器；v0.27b 写-编-跑演示时暴露）

* **现象**：`writefile` 写入的源码被 shell 的 `ARG_MAX=32` 截断，形参列表
  `int syscall3(int n,int a,...` 在 EOF 处未闭合。编译该残缺源码时编译器**死循环**：
  反复调用 `sym_declare("")`，符号表 `table_pos` 每次 +6 无界增长，约 600 次后
  `table[0xef8]` 写越出映射页 → `PAGE FAULT @801ad000 eip=… eax=801ac108 ebx=ef8`。

* **根因**：`program()` 形参解析 `while (accept(")") == 0) { … type_name(); … }`
  在 token 变为空（EOF）时 `accept(")")` 恒假、`type_name()` 也只会再读到空，
  `sym_declare(token, …)` 以空名字被无限调用——**缺 EOF 终止/报错守卫**。
  原版 cc500 只在完整源码上自举，从未遇到畸形输入。

* **修复**：`program()` 在"缺名字处遇 EOF"与"形参列表遇 EOF"两处补 `if (token[0]==0) error()`
  （畸形输入直接 `exit(1)`，不再死循环）。同时 shell `ARG_MAX` 32→128 解除 writefile 内容截断。

* **回归**：`make test` 全绿；writefile+ccrun 写-编-跑用例通过；ccboot 自举不动点不受影响。

* **教训**：解析器对"输入流在未预期位置耗尽"必须有显式报错路径，不能依赖
  `accept()` 永远能匹配到终结符；自托管编译器的健壮性边界要用**畸形输入**用例覆盖，
  而不只是"能编译自己"。

***

## BUG-027 \[已修复] v0.28 sys\_map\_page 记账槽满时映射帧泄漏

* **版本**：v0.11 引入（v0.28 代码审查发现）

* **现象**：进程第 9 次调用 `sys_map_page` 时，新帧被映射进地址空间但**未记入**
  `pcb_t::map_frames[8]`（`if (p_ && p_->map_fcount < 8)` 静默跳过记账）。
  进程退出时 `release_priv_frames` 只释放 `map_frames[0..map_fcount-1]`，
  第 9+ 帧永久泄漏。

* **根因**：`map_frames[8]` 是固定数组，`sys_map_page` 在分配/映射后才发现槽满，
  没有回滚（unmap）也没有拒绝，账实不符。

* **修复**：记账槽满在**分配前**拒绝——`map_fcount >= 8` 直接返回 -1，不分配不映射
  （`usermode.c` case 23）。诚实暴露"每进程至多 8 张私有映射页"的教学上限，
  而非悄悄泄漏。

* **回归**：`make test` 全绿；isol 演示（1 张映射页）不受影响。

***

## BUG-028 \[已修复] v0.28 exec 路径泄漏 load\_frames 记账数组

* **版本**：v0.26#3 引入（`load_frames` 由固定数组改 kmalloc 动态数组时漏了 exec 路径）

* **现象**：每次成功/失败的 `sys_exec` 都泄漏 `load_frames`（kmalloc 的
  `load_maxframes × 4` 字节数组）。频繁 exec 会缓慢蚕食内核堆。

* **根因**：`usermode_spawn_elf`（spawn 路径）在成功/失败后都调 `load_frames_free()`
  （v0.26#3 补上了），但 **exec 路径（case 25）漏加**：成功路径 `sched_exec` 不返回、
  失败路径只释放帧没释放数组。`sched_exec` 把 `load_frames` **复制**进 PCB 的
  `own_frames`（新 kmalloc 数组）后，源数组成了孤儿。

* **修复**：

  * `sched_exec` 复制完 `frames` 后 `kfree(frames)`（源数组为调用方临时记账数组，
    OOM 提前返回时由调用方负责）；

  * exec 失败路径补 `load_frames_free()`。

  * 接口：`sched_exec` 的 `frames` 参数由 `const uint32_t *` 改为 `uint32_t *`
    （语义=调用方移交、sched\_exec 复制后释放）。

* **回归**：`make test` 全绿；`exec args hello world`、`exec nosuchprog` 用例通过
  （exec 成功与失败两条路径都覆盖）。

***

## BUG-029 \[已修复] v0.29 icmp\_parse 短帧越界读（宿主 fuzz 抓到）

* **版本**：v0.23 引入（v0.29 宿主 fuzz 发现）

* **现象**：fuzz 对 `icmp_parse` 注入随机短帧时，ASan 报堆缓冲区越界读。

* **根因**：`icmp_parse` 开头直接 `ip_parse(frame + 14, len - 14, ...)`：

  * `len < 14` 时 `frame + 14` 已越过帧尾；

  * `len - 14` 是无符号下溢成巨大值（约 42 亿），`ip_parse` 按该长度扫描"载荷"
    → 越界读。

* **修复**：`icmp_parse` 开头加 `if (len < 14) return -1;`（与 `udp_parse` 同款守卫），
  杜绝下溢与越界指针（`src/net/icmp.c`）。

* **回归**：`test_icmp.c` 补 13/0 字节短帧断言（`icmp_parse(frame, 13, ...) == -1` 等）；
  宿主 fuzz 60k 轮 + 200 万轮复核无崩溃；五层回归全绿。

***

## BUG-030 \[已修复] v0.29 fork 子进程在继承的已生长栈上继续递归被误判缺页

* **版本**：v0.12 引入（fork 深拷贝栈）；v0.29 回归盲区补格（deepfork 组合）暴露

* **现象**：父进程递归 12KB 使栈按需生长后 fork；子进程在继承的已生长栈上继续递归
  下探时，子进程被以 `code=0xFFFFFFFF` 终止（普通页错误路径），未能触发栈按需生长。

* **根因**：`sched_fork` 深拷贝父进程全部栈页时，把子进程的 `user_esp_top`/
  `stack_bottom` **原样继承**——子进程的栈虚拟地址落在**父进程的栈槽**，而非自己的槽
  （forkdemo 因浅栈不生长而从未触发）。而 `stack_guard_hit` 用**子进程 pid** 反推槽位：
  子进程下探时 fault 落在"按子 pid 推导的槽外"，被判 `STACK_OK`（非栈事件），
  走普通缺页路径被隔离终止。

* **修复**：`stack_guard_hit` 槽位改由**实际栈位置** **`stack_bottom`** 推导
  （`stack_bottom & ~(USER_STACK_SLOT-1)`），而非 pid——普通进程=自身槽、
  fork 子进程=继承的父槽，两者皆正确；v0.15"下一槽边界不误报"语义由真实栈槽天然保持。
  `pid` 参数保留但不再参与判定（`src/kernel/guard.c`）。

* **回归**：`test_guard.c` 补 4 条 fork 继承栈断言（旧逻辑下必失败）；新演示
  `deepfork`（已生长栈×fork）与 `deepexec`（已生长栈×exec）挂入 qemu\_regression.sh /
  test\_serial.sh；串口日志确认子进程在继承栈上继续 `[stack] grow pid=4` 两次并存活，
  父进程 wait 回收 `code=0`。

***

## BUG-031 \[已修复] v0.30 全局文件槽泄漏：一次编译失败污染整条工具链（=用户报告 BUG-A）

* **版本**：v0.8（固定槽 fs 模型）引入；v0.29 独立实操复现（`tests/repro_bugs.sh`）

* **现象**：cc500 编译语法被拒的源（parse error → `error()` = 裸 `exit(1)`）之后，
  再编译与最初**字节级一致**的源也会失败（`cc500: output setup fail` / `[ccrun] compile
  FAIL code=1`），直至重启。同一输入判若两机。

* **根因**：`fs_files[8]` 是内核**全局表、无进程归属**（`usermode.c` 的
  `static fs_file_t fs_files[FS_MAX_OBJ]`）；`terminate_current`/`reap_process`
  退出路径**不清理**任何槽。cc500 `setup_output` 打开 slot2 后，唯一归还点是成功路径的
  `flush_output`；parse error 直接 `exit(1)` 跳过它 → slot2 永久占用 → 此后任何 cc500
  的 `setup_output`（`SYS_FS_OPEN slot2` 因 `fs_files[2].used==1` 失败）→ 工具链被污染
  直到重启。

* **修复**：文件槽记**打开者 pid**（`fs_file_t.pid`，`sys_fs_open` 写入），进程退出时
  `fs_files_close_pid(pid)` 只归还该进程的槽（`terminate_current` 调用）。首版"关闭全部
  槽"在 boot 演示进程与 shell 编译并存时会误伤并发进程（repro 抓到：procSemB 退出把
  P1 正写 /out.elf 的 slot2 也关了）——按 pid 归属清理杜绝此问题。

* **回归**：`tests/repro_bugs.sh`：bad.c 编译 FAIL 后，字节级同源 good2.c 再编译成功
  （`[ccrun] '/good2.elf' exited code=0 PASS`）；五层回归全绿（含 ccboot P1==P2）。

* **后续架构债**：per-process fd 表（打开文件表入 PCB）仍留 roadmap——本修复是
  "无归属全局表"之上的最小止损。

***

## BUG-032 \[已修复] v0.30 cc500 自编译产物静默丢失 exec argv（=用户报告 BUG-B）

* **版本**：v0.27（be\_start 入口桩）引入；v0.29 独立实操复现（`tests/repro_bugs.sh`）

* **现象**：`exec /out.elf /cc500.c /out2.elf`（`/out.elf` = cc500 自编译产物 P1）
  静默忽略 argv，用默认路径写 `/out.elf` 且退出码 0（不报错）；`/out2.elf` 从未创建。

* **根因**：cc500 `be_start` 发射的入口桩 = 裸 `call <首函数>; mov %eax,%ebx; xor %eax,
  %eax; int $0x80`，**不编组 argc/argv**。内核以 `[esp+4]=argc、[esp+8]=argv` 进入桩，
  `call` 再压一个返回地址 → 首函数读到 `[esp+4]=fake_ret(0)`、`[esp+8]=argc` → 形参
  全错位（argv 形参读到 argc 值、argc 形参读到 0）→ `main1` 的 `3<=argc` 不成立 →
  走默认路径。gcc 构建路径的 crt.c / cc500\_crt.c 正确编组 argc/argv，故"命令行路径"
  仅对 **gcc 版 cc500** 成立；ccboot 固定路径、ccrun 用 gcc 版，都在回归矩阵之外，
  从未触发（v0.27b 的 "✅ 命令行路径" 对自编译产物不成立）。

* **修复**：`be_start` 入口桩在 `call` 前把内核栈上的 argv/argc 压给首函数：
  `mov eax,[esp+8]; push eax`（argv）→ `mov eax,[esp+8]; push eax`（argc）→ `call`。
  与 cc500 约定"首参 8(%esp)/末参 4(%esp)"精确对齐（首函数须声明为 `(char *argv,
  int argc)`，cc500.c 即如此）；无参首函数不受影响。`e_entry` 不变（0x800A0054，
  首指令），`call` 的 rel32 回填偏移由 85→95（`save_int(code+95, codepos-99)`）。

* **回归**：`tests/repro_bugs.sh`：ccboot 产 P1 → `exec /out.elf /cc500.c /out2.elf`
  → `/out2.elf` 被创建（19217 字节，`[ls] out2.elf size=19217`）；ccboot P1==P2
  逐字节一致仍成立（新桩在自举两代产物中一致）；五层回归全绿。

***

## BUG-033 \[已修复] v0.30 map\_page\_in 页表帧 OOM 时写物理 0 破坏内核（代码审查发现）

* **版本**：v0.11（`map_page_in` 引入）潜伏；代码审查静态发现（P0）

* **现象/根因**：新建页表（`dir[pd_idx]&1==0`）时调用 `frame_alloc()` 未检查返回值。帧池
  地址从 1MB 起，0 只代表 OOM；旧代码拿到 0 仍继续 `(uint32_t*)0` 清零 4096 字节 → 破坏
  低 4KB（保护模式下虽未映射，但页目录项会被写成 `0x7` 指向物理 0 的"合法"页表），且
  `pf_handler` 栈生长路径无法感知失败：帧已记账、`stack_bottom` 已下移但页未映射 →
  iret 重试同地址再次 fault → 反复空耗物理帧（假增长）。

* **修复**（`src/mm/mem.c` + `src/mm/mem.h`）：`map_page_in` 返回类型 `void`→`int`
  （0 成功 / -1 页表帧 OOM 未建立映射）；`pf_handler` 栈生长检查返回值，失败时释放刚
  分配的栈数据帧并转 `STACK_BOOM`（不再假增长）。其余调用方（boot/懒分配/elf 加载/
  spawn/exec）忽略返回值语义不变。

* **回归**：全量五层回归绿（宿主 16/16、QEMU、串口、持久化、网络）。

***

## BUG-034 \[已修复] v0.30 kb 行缓冲在 line\_ready 期间仍追加输入，两行可能合并（代码审查发现）

* **版本**：v0.9（行缓冲引入）潜伏；代码审查静态发现（P2）

* **现象**：`line_ready==1`（有未取走的行）时，新输入的可打印字符仍追加到 `line_buf`
  末尾。若外部 agent 高速注入多行（如串口脚本 `"abc\nxyz\n"`），`"xyz"` 追加到
  `"abc"` 后，`kb_line_take` 取走首行重置 `line_len` → `"xyz"` 丢失。

* **修复**（`src/drv/kb.c`）：仅当 `!line_ready` 时才把可打印字符入行缓冲。

* **回归**（`tests/test_kb.c` 用例 13）：行就绪未取时输入 `'b'/'c'` 被忽略，取回首行
  仍为 `"a"`。

***

## BUG-035 \[已修复] v0.30 fork\_frames\[24] 硬编码上限限制大进程 fork（=OBS-002）

* **版本**：v0.12（`sched_fork` 深拷贝引入）潜伏；代码审查确认 OBS-002 属实（P1）

* **现象**：`pcb_t::fork_frames[24]` 固定 96KB 记账数组，深拷贝超 24 页即 `goto
  fork_oom`。用户进程最多可拥有栈 7 + ELF ≤256 + 堆 80 + map 8 ≈ 351 页；bigdemo
  （21 帧 + 栈 7 = 28）已超上限，大进程 fork 必然失败。

* **修复**（`src/kernel/sched.h` + `src/kernel/sched.c`）：与 `own_frames` 同策略——
  `fork_frames` 改 `uint32_t *` 动态数组；`sched_fork` 先数一遍需深拷贝页数再按需
  `kmalloc`，`release_priv_frames`/`fork_oom` 中 `kfree`。

* **回归**：全量五层回归绿（宿主 16/16、QEMU、串口、持久化、网络）。

***

## BUG-036 [已修复] v0.30 Makefile cc500 豁免只留 `-w`：GCC 14 下 `-Wint-conversion` permerror 漏网

* **版本**：v0.30（L-5 清理时引入）；评估方 GCC 14 环境实测触发

* **现象**：L-5 认为 `-w` 全量覆盖、删掉显式 `-Wno-int-conversion` 只留 `-w`。
  但 GCC 14 把 `-Wint-conversion` 从 warning 升级为 **permerror（硬错误）**，
  `-w` 只抑制 warning 压不住 error → `make clean && make` 在 cc500.c 编译处
  报 `-Wint-conversion` ×13、EXIT=2 构建失败。
  本地 GCC 13 验证通过（`-Wint-conversion` 仍只是普通 warning、`-w` 够用）——
  是"环境依赖"的验证盲区：L-5 清理只在本机 GCC 13 验证，未覆盖 GCC 14 语义。

* **修复**（`v2-c-kernel/Makefile` cc500 单文件规则）：恢复 `-Wno-int-conversion`
  并与 `-w` 并存（GCC 8+ 两者均有效）。GCC 14 需要它压 permerror；`-w` 继续
  压其余普通警告（`-Wcompare-distinct-pointer-types` 的 `in_data == (0-1)` 惯用法、
  `-Wmaybe-uninitialized`）。

* **验证**：用户最小样例三分隔离——`-w`→exit 1、`-w -Wno-int-conversion`→0、
  `-w -fpermissive`→0；本沙箱 GCC 13 下 `make clean && make` 零告警产出 kernel.elf，
  宿主 16/16 + QEMU 回归全绿。

* **教训**：`-w` 不是 `-Werror` 的反义词（只压 warning 不压 permerror）；GCC>=14
  默认 error 化的诊断（-Wint-conversion、-Wincompatible-pointer-types 等）必须显式
  `-Wno-*`。跨编译器版本验证时，构建矩阵应至少覆盖"默认 error 化"那一档。

***

## BUG-037 \[已修复] v0.31 socket 表退出泄漏：开 socket 不关即退出，槽位永久失踪（=F-0a）

* **版本**：v0.31（socket 归属收口时）

* **现象**：进程 open UDP socket 后不 close 直接退出，`netsock` 表槽位被永久占用；反复
  "开-退" 多个进程后表满（`NET_SOCK_MAX` 含 DHCP 专用 1 槽），网络调用 `socket()` 返回 -1，
  网络功能降级，直到重启才恢复。

* **根因**：`net_sock_t` 无进程归属记录（打开者为谁、随谁回收），且进程退出路径没有任何
  socket 回收逻辑——`terminate_current` 只清理 fd/私有帧/信号量，漏掉 netsock 槽。

* **修复**（`v2-c-kernel/src/net/netsock.c/h` + `src/kernel/sched.c`）：`net_sock_t` 增
  `pid` 字段（`netsock_open` 记 `sched_current_pid()`）；`terminate_current` 调新增
  `netsock_close_pid(pid)` 归还该进程所有 socket 槽。

* **回归**：`test_socket.sh` F-0a——leak2 在同进程循环 open 5 次（表净可用 3 槽）退出，
  断言修复后泄漏链不出现、`netping` 仍能开 socket 收到 PONG；修复前该场景 `socket() FAIL`。

***

## BUG-038 \[已修复] v0.31 任意 close：可关闭内核 DHCP 保留槽，续约链被打断（=F-0b）

* **版本**：v0.31（socket 归属收口时）

* **现象**：任何进程可 `close(任意 socket)`，连内核 DHCP（端口 68）专用槽也能被关；关闭后
  DHCP 续约 RENEW/REBIND 无人收 ACK，租期一到被逼回退静态 IP，网络配置静默降级。

* **根因**：`case 33`（`sys_net_close`）裸调 `netsock_close(id)`，无归属校验、无保留标记；
  用户进程可越权关闭它没打开的、甚至是内核专用的 socket。

* **修复**：`net_sock_t` 增 `reserved` 标志，`netsock_dhcp_open` 标记 DHCP 槽 `reserved`+`pid=0`；
  `case 33` 改调 `netsock_close_if_owner(id,pid)`——仅允许关闭"本 pid 打开且非保留"的槽，
  否则 `DENIED`。

* **回归**：`test_socket.sh` F-0b——closer 对 `close(0)`（DHCP 保留槽）被拒、运行仍 PASS、
  攻击后续约 ACK 照常出现、无 "lease lost" 静态回退；审计 `netsock_audit` 恒报保留槽计数。

***

## BUG-039 \[已修复] v0.32 cc500 未闭合字符串字面量：越界读写自噬，被内核击杀（=F-3）

* **版本**：v0.32（cc500 缺陷收口时）

* **现象**：源码里字符串字面量未闭合（EOF 前无终止引号，如 `sys_print("unterminated` 到文件尾），
  cc500 编译时内部越界读/写，guest 下被内核击杀（`PAGE FAULT ... -> killed` → `compile FAIL
  code=4294967295`）、宿主 hostcc SIGSEGV（rc=139）。错误形态随机（砸进 brk arena 相邻分配）。

* **根因**（两层，首崩点在 get\_token 而非仅 primary）：

  1. `get_token` 字符串读取 `while (nextc != '"') takechar();` 无 EOF 守卫（CC500 子集无
     `break`，只能靠标志变量）——未闭合输入到 EOF 仍死循环，`token`（my\_realloc 动态缓冲）
     无限增长直至越界；
  2. `primary_expr` 解码 `while (token[j] != '"')` 无 NUL 守卫——越过 token 尾部 NUL 继续读
     直到堆里偶遇 `"` 字节，且同循环 `token[i]=` 写越界。

* **修复**（`v2-c-kernel/tools/cc500/cc500.c`）：get\_token 字符/字符串读取加 EOF 守卫
  （`if (nextc==0-1) 置标志`）；primary\_expr 解码 while 加 `token[j]!=0` 守卫，命中 NUL 未闭合
  即 `sys_print("cc500: bad string\x0a"); error();` 干净报错退出 1。

* **回归**：宿主 `t6` rc=1 且日志含 `bad string`（修复前 rc=139 SIGSEGV）；guest ccrun 报
  `code=1` 而非 `code=-1(被击杀)`。

***

## BUG-040 \[已修复] v0.32 cc500 只声明未定义函数：静默编出 "call 自身 ELF 头" 的废产物（=F-2）

* **版本**：v0.32（cc500 缺陷收口时）

* **现象**：`int sys_print(char*s);`（仅原型声明、无同文件定义）编译"成功"输出 `cc500:
  compiled OK`；但产物运行时即 `PAGE FAULT ... -> killed`，eip 落在 0x800A0000——把产物自己的
  ELF 头当代码执行（hostcc 产物内含 `mov eax,0x800A0000`+`call` 模式）。编译期零报错零告警。

* **根因**：`sym_declare_global` 对首次出现符号记 `class='U'`、`value=code_offset(0x800A0000)`；
  引用点经 'U' 链等 `sym_define_global` 回填。但"纯原型声明"走 `program()` 的
  `accept('(')` 函数声明分支，`accept(';')` 为真时**不调用** **`sym_define_global`**——定义永不到来，
  无人回填，调用目标恒留 `0x800A0000`。

* **修复**（`v2-c-kernel/tools/cc500/cc500.c` `be_finish`）：编译收尾遍历全局符号表，检出残留
  `class='U'` 且 `value != code_offset`（被引用未回填）即 `cc500: undefined symbol` + `error()`。
  **遍历锚点是"名字 NUL 位置"**（同 `sym_define_global` 的 `table[t+1]=class` 语义），非符号起点。

* **回归**：宿主 `t4` rc=1 且日志含 `undefined symbol`（修复前 rc=0 `compiled OK`）；负对照
  "声明 + 同文件定义" rc=0，无谓误报；ccboot 自举 P1==P2 仍成立（cc500 自身的库声明均有同文件定义）。

***

## BUG-041 \[已修复] v0.32 cc500 关系运算残缺（只有 <=）+ error() 零诊断（=F-1）

* **版本**：v0.32（cc500 缺陷收口时）

* **现象**：`relational_expr` 只识别 `<=`，`<` / `>` / `>=` 全部缺失——教程式 `while(i<n)` 静默
  parse error 退出 code=1 零输出；`error()` 是裸 `exit(1)`，不打印任何位置/token，学员无从归因。

* **根因**：`relational_expr` 只有 `while (accept("<="))` 一个运算符；`error()` 无上下文参数。

* **修复**（`v2-c-kernel/tools/cc500/cc500.c`）：

  * `error()` 升级为打印 `sys_print("cc500: error at\x0a"); sys_print(token); sys_print("\x0a");`
    （新消息用 `\x0a`——CC500 字符串解码此前只解 `\xNN`，故已为解码补 `\n`/`\t` 常规转义）；

  * `relational_expr` 补齐 `<`/`>`/`>=`（与 `<=` 同构，仅 setcc 字节不同：setle=0x9e / setl=0x9c /
    setge=0x9d / setg=0x9f；操作数序 `pop %ebx; cmp %eax,%ebx` 不变，`setl` 编码 0f 9c 由 objdump
    实测锁定）。

* **回归**：宿主 `<`/`>`/`>=`/`<=` 各 compile OK + `<` 编码 0f 9c 锁定；guest `<` 运行语义
  （`while(i<1)` 恰循环 1 次、`i==1` 返回 0 即 exit code 0，方向错则会返回 1）；cc500 自举不动点与
  原有 `<=` 用法不受影响。

***

## BUG-042 \[已修复] v0.33 selftest 汇总行可被内核异步打印撕裂 → 回归假阴性（=F-4）

* **版本**：v0.33（回归抗撕裂收口时）

* **现象**：`cmd_selftest` 的 `[selftest] PASS (N checks)` / `FAIL` 汇总行被内核异步打印（如孤儿
  reap、DHCP 短租期续约）撕裂成三截——现场实录 `[selftest] PASS ( ... check ...` 拆断命中。
  所有以"整行 PASS"为锚的断言在"boot 演示未退出 + 续约打印"窗口下可随机漏匹配（harness 已实
  被咬过 30s 假超时）。

* **根因**：`src/app/shell.c` `cmd_selftest` 汇总用多次 `sys_print` 片段拼行（`PASS (` + 计数 +
  ` checks)`），片段间可被任意其他上下文插入；而对 netping/ccboot/writefile 早已用 `nl_*` 缓冲
  原子行（单次 `sys_print` flush），selftest 却漏用。

* **修复**：`cmd_selftest` 汇总改走 `nl_reset/nl_s/nl_u/nl_end` 一次缓冲 + 一次 flush（对前向
  声明位于文件后部的 `nl_*` 加声明）。**未改内核串口全局原子性**（cli/sti 包行是另一设计决定）。

* **回归**：test\_socket.sh 增 F-4 撕裂探测器（`[selftest] PASS (` 开头却非整行 `checks)` 结尾的
  残缺行计数=0），短租期 DHCP 续约并发窗口下 0 撕裂；连发 selftest 全为整行。

***

## BUG-043 \[已修复] v0.33 pid 表耗尽静默返回，无任何日志（=F-5）

* **版本**：v0.33（可观测性收口时）

* **现象**：pid 表（`MAX_PROCS` 槽）耗尽时 `alloc_pid` 静默 `return -1`，三处调用方
  （`sched_spawn`/`spawn_at`/`sched_fork`）对 `pid<0` 无声返回。A4 fork 炸弹实测"成功 29 条、
  FAILED 0 条"——先到达的约束是**无声的槽耗尽**而非有日志的深拷贝 OOM。教学场景学生面对
  "spawn 失败但内核什么都不说"。

* **根因**：`src/kernel/sched.c` `alloc_pid` 无失败日志（OOM 分支 `spawn FAILED` 有日志，槽耗尽
  分支无——不对称）。

* **修复**：`alloc_pid` 增 **每耗尽周期报一次**的 `[sched] pid table full`（有 free 槽时重置标志，
  防 spawn/bomb 风暴刷屏）；`sched_audit` 汇总行补 `slots=%u/MAX_PROCS`。

* **回归**：fork 炸弹实测日志恰 1 条 `pid table full`（防刷屏生效）、无 panic/无重启；审计行含
  slots 计数。

***

## BUG-044 \[已修复] v1.1 Step 4 大响应接收"容量丢字节"假象为"回绕区损坏"（B1/B2/E2 合并）

* **版本**：v1.1（netif Step 4 薄包装 + 转发器）；独立核验报告指认后 dev 逐项核实修正

* **现象**：HTTP 响应 >1KB 时 `tcp_recv` 要么超时 -1、要么结尾字节在**固定的位移窗口**被"替换/
  缺失"（确定性复现，与内容/节奏/ISR 无关）。核验报告将其定性为"1024B 环回绕区地址损坏"，并
  称"把 TCP\_RXB 1024→2304 即完全消失"为证据。

* **核实再判**：**根因不是回绕区地址损坏，而是同族三处容量上限叠加导致丢尾**：

  1. `NET_RXMAX=512`：netsock `dispatch_frame` 对 >512 的数据报静默 `plen=NET_RXMAX` 截断；
  2. 转发器下行**不分块**：`_tcp_read` 把 TCP 读到的整段（≤4096B）塞进单条 `MSG_DATA`，
     guest 单数据报根本收不下——而 guest 的 `sendto/recvfrom` 在系统调用层都钳制 1400；
  3. 接收环 `TCP_RXB=1024` 被分块突发写满后 `rx_push` 丢尾部字节（响应缺缝）。
     环用 `(x) % TCP_RXB` 对任意环长成立，**不存在 power-of-2 掩码回绕 bug**；"换 2304 即全清"恰好是
     环体量 > 最大逻辑突发才不丢——是**容量证据，不是地址证据**。

* **修复**：NET\_RXMAX 512→2048（≤1400 数据报不被 dispatch 截断）；TCP\_RXB 1024→4096；转发器下行
  单条 MSG\_DATA 分块 ≤1392；tcp\_send 发送硬墙对齐传输真实上限（TCP\_MTU 1472→1400，盖过
  1472/1600 名目值）。收、发、转发三处是**同一份约束的三个落点**，一次相干改动而非逐条打补丁。

* **回归**：httpdemo 拉 2000B />1KB 响应，断言 `len>1024` 且尾字节 ==`TAIL`（分块/`NET_RXMAX`/环
  任一丢字节都会 MISS）；test-tcp 双通道 + host 19/19 + 默认内核构建无告警。

* **教训**：核验报告把容量症状误判为地址损坏时，dev 要复核根因再修；"换大容量就消失"是**容量**
  证据，与"地址/回绕语义错误"是两类病；环类实现用 `%` 而非掩码可排除相邻嫌疑，集中排查容量链。

***

## BUG-045 \[已修复] v1.1 Step 4 转发器主循环阻塞 recv 饿死串口输入（SLIP 通道失败根因）

* **版本**：v1.1 Step 4（`tests/tcp_proxy.py`）

* **现象**：UDP（e1000）通道虚拟 TCP demo 全绿；但串口（COM2/SLIP）通道 httpdemo 完成 open、
  发出 GET 后 `tcp_recv` 超时 -1、无 200 OK；转发器日志停在 `OPENED sid=1`，无下行 DATA。

* **根因**：`_service_tcp` 对 TCP 下行 socket 用 `settimeout(8s)` **阻塞 recv**；转发器是单线程
  主循环，阻塞在 `_service_tcp` 期间不再 `select` chardev/udp 输入。guest 在 OPENED 后经 SLIP 发来的
  GET 帧被滞留在 OS 缓冲无人读；而 guest `tcp_recv` 超时上限 5s **小于**转发器 8s，guest 先超时，
  之后即便转发器读到也已来不及。

* **修复**：TCP 下行 socket 改**非阻塞**，主循环用 `select` 同时监听 chardev/udp 与所有 OPEN 的
  TCP socket，从就绪集非阻塞读（B1 分块也在此落点，见 BUG-044）。主循环永不被单条 recv 卡死。

* **回归**：SLIP 通道 httpdemo 拉到 200 OK + `RESULT PASS`；test-tcp A/B 双通道断言全绿。

* **教训**：单线程事件循环里**任何阻塞 I/O 都会饿死其他通道**；读下游 TCP 必须非阻塞 + multiclient
  select，且各超时（guest 5s vs 转发器 8s）要协调，否则上游先超时。

***

## BUG-046 \[已修复] v1.1 Step 4 收尾 test\_tcp.sh 自检无等待窗 → CI 稳定误报"HTTP 服务未就绪"

* **版本**：v1.1 Step 4 收尾（PR #16，审核方复验指认）

* **现象**：PR #16 test 首跑失败、**rerun 同一 commit 复验仍红**（非 flaky——审核方定义
  "rerun 转绿才叫 flaky，这次没转绿"）；失败点 `[FAIL] HTTP 服务未就绪（端口被占或绑定失败）`；
  7×layer 全绿、内核 16 项单测过、宿主 19/19 pass fail=0，旁证 exit code 0。

* **根因**：PR #16 自身把 `python3 -m http.server + sleep 0.5`（**有启动等待窗**）换成内联
  python socket 服务 + **单次无等待 curl** 存活自检。CI runner 冷启动 python 解释器慢时，curl
  先到、服务未 `listen()` → `ECONNREFUSED` → 稳定误报"未就绪"；且 `run_http_server` 失败后
  脚本不退出，后续 `await_log` 全部超时 → 断言连锁红。

* **修复**：`run_http_server` 自检改**重试循环**（`curl && break`，间隔 0.5s ×10 ≈ 5s 等待窗），
  持续失败才报"未就绪"；两处调用改 `run_http_server || exit 1`：服务起不来立即红并退出，不再
  带病跑完制造连锁误报。

* **回归**：`bash -n` 通过；模拟"慢启 1.2s + 重试轮询"单验自检正确等到服务就绪 200；双通道全量
  待 qemu 环境复跑确认。

* **教训**：宿主测试脚本**自改的时序点就是 CI 假红的头号来源**——改动涉及"等外部服务就绪"时
  必须保留/引入等待窗与重试，不能把等待删成"单次无等待探测"；服务未就绪要 fail-fast（exit）
  而非继续跑完连锁红。

***

## BUG-047 \[已修复] v1.2 虚拟 TCP 大响应丢尾：drain 全量 pump 挤爆 rxb 环

* **版本**：v1.1 Step 4（薄包装 + 转发器）；v1.2 覆盖率扩展测试发现

* **现象**：HTTP 响应体 ≥~4096B（含头部）时，guest 收到的响应尾字节 `TAIL` 缺失，
  `[http] HTTP 200 OK closed=1 len=4096 tail=MISS -> RESULT FAIL`。8KB/32KB 响应同丢尾；
  官方 2000B 测试从未触发。e1000 与 SLIP 两通道皆然（与通道无关）。

* **根因（分层定位）**：

  1. `tcp_recv` 每次循环**先** **`drain()`** **全量 pump**：`drain` 用 `for(;;)` 把 netsock 队列里
     转发器下行**所有**分块一次性 `rx_push` 进 `rxb[TCP_RXB=4096]` 环；
  2. `rx_push` 环满（`next==rx_head`）时 `return` **丢弃剩余字节**；
  3. `httpdemo` 自身两个 `TCP_RXB` 上限（`resp[TCP_RXB+1]` + `rlen < TCP_RXB`）再加一层截断。
     于是"一次性到达的全部下行（头部+正文）> 4096 环"时，第 \~3 个数据报之后的字节在 drain
     阶段即被永久丢弃，`tcp_recv` 只读到前 4096，尾部缺失。此为 BUG-044 容量收口之后的**更高下界**：
     转发器分块 ≤1392、NET\_RXMAX=2048、TCP\_RXB=4096 三者仍未覆盖"响应含头部超 4096"的场景。

* **核实要点**：`drain` 的 `for(;;)` 全量 pump 是关键——它把"UDP 队列里已到的 N 个分块"一口气
  塞进一个固定 4096 环，溢出即丢；而非"读一个、消费一个"的背压节奏。若把 drain 改为
  **每次只 pump 一个数据报**，环内同时驻留 ≤ 一个数据报 + 未读走残留，即可根治（配合调用方循环读）。

* **修复（已落地）**：

  * `tcp.c` `drain()` 全量 pump → **每次只从 netsock 取一个会话数据报**（节流/背压）；

  * `tcp.c` `tcp_recv()` 改为**先读环、环空才 pump 一个、再读**，避免环被单次全量填满；

  * `tcp.h` `TCP_RXB` 4096→16384（环体放量，供更大响应 + 单数据报 1400 与读缓冲余量）；

  * `httpdemo.c` 响应缓冲改 `static`（BSS，避免大栈帧）且跟随 `TCP_RXB`，读缓冲配套。

* **回归（已补）**：test-tcp 增"8KB 响应尾字节完整"断言；e1000/SLIP 双通道 `tail=TAIL` 均通过。

* **教训**：固定环 + 无背压的"全量 pump"是丢数据的结构根因；"把环调大"只是抬上限，
  根治须让生产（drain）与消费（recv 读）同步，即在环空时才 pump。

***

## BUG-048 \[已修复] v1.2 cc500 块注释未闭合 → 编译器死循环（=F-7）

* **版本**：v0.27（cc500 编译器）；v1.2 编译器审计 fuzz 发现

* **现象**：源码含未闭合的块注释（如 `int main(){/*` 到 EOF 无 `*/`）时，cc500 编译**死循环**
  （宿主 hostcc 挂起、`timeout` 返回 124；guest ccrun 无响应）。畸形输入 fuzz 500 例命中
  **12 个 TIMEOUT，全部含未闭合** **`/*`**，且无 segfault/越界崩溃（崩溃健壮性尚可，唯死循环）。

* **根因**：[cc500.c L128-139](../../v2-c-kernel/tools/cc500/cc500.c) `get_token` 块注释循环
  `while (nextc != '/') { while (nextc != '*') nextc = getchar(); nextc = getchar(); }`
  **无 EOF 守卫**。`getchar()` 到 EOF 恒返回 `-1`，而 `-1 != '*'`(42) 恒真 → 内层死循环。
  这是 F-3（未闭合字符串，L106-114 已加 `nextc==0-1` 守卫）的**同款 bug**，但块注释路径漏修。

* **修复（已落地）**：块注释读取加 EOF 守卫（与字符串/字符字面量同款标志变量，C 子集无 `break`）：
  读 `*` 循环命中 EOF（`nextc==0-1`）即置标志、跳出并走 `error()` 干净报错。

* **回归（已补）**：hostcc 未闭合注释 `rc==1`（修复前死循环超时）；`int main(){/* ok */}` 闭合注释
  仍 `rc==0`；fuzz 复核无 TIMEOUT（`test_cc500.sh` `t_bcomm` 用例 + timeout 兜底）。

* **教训**：自托管编译器对"输入在未预期位置耗尽"要**处处**有 EOF 守卫——字符串已守、块注释漏守；
  解析器健壮性边界须用畸形输入 fuzz 覆盖，而非只"能编译自己"。

***

## BUG-049 \[已修复] v1.2 cc500 数字+字母混合字面量被静默算错（=F-8）

* **版本**：v0.27（cc500 编译器）；v1.2 编译器审计边界用例发现

* **现象**：`return 0x10;` **编译成功且不报错**，但产物把 `0x10` 当十进制串 `0x10` 里的字符
  `'x'`(120)-`'0'`(48)=72 参与进位，得到 **7210** 而非 16 或报错（`123abc`、`9_z` 同类均静默错值）。

* **根因（两层叠加）**：

  1. [cc500.c L97-99](../../v2-c-kernel/tools/cc500/cc500.c) `get_token` 把**字母/数字/下划线吃成同一
     个 token**（循环条件含 `('0'<=nextc&nextc<='9')` 与 `('a'<=nextc&nextc<='z')`），故 `0x10` 是
     "一个 token"；
  2. [cc500.c L407-413](../../v2-c-kernel/tools/cc500/cc500.c) `primary_expr` 数字解析只校验 `token[0]`
     是数字就进 `while(token[i]) n=n*10+token[i]-'0'`，**不校验后续字符全为数字** → `'x'` 被当数码。
     "垃圾进、静默错出"，比"干净报错拒绝"危害更大（学员难归因）。

* **修复（已落地）**：数字字面量解析时校验每个字符 `'0'<=c&c<='9'`，命中非数字即 `error()` 干净
  报错退出（十六进制 `0x` 与混杂标识符 `123abc` 均明确拒绝）。

* **回归（已补）**：hostcc `0x10`/`123abc` 应 `rc==1`（干净拒绝）而非 `rc==0` 产错值；纯十进制
  `123` 仍 `rc==0`；cc500 自举不受影响（源码无此类字面量）。`test_cc500.sh` `t_mixhex`/`t_mixalpha`
  用例锁死。

* **教训**：词法的"标识符/数字"分界一旦模糊（字母数字混吃），必须在下游**数值解析处设防御校验**；
  "不接受"要"显式拒绝并报错"，绝不"接受后按错误语义往下算"。

***

## BUG-050 \[已修复] v1.2 虚拟 TCP e1000 通道丢 CLOSED：netsock 全量排空挤爆 socket 环

* **版本**：v1.2（BUG-047 修复后、双通道复测发现）

* **现象**：BUG-047 修复后，SLIP 通道全绿，但 **e1000（UDP）通道**仍 `[http] HTTP 200 OK closed=0
  len=8252 tail=TAIL -> RESULT FAIL`，并伴随 `[http] recv ERR (unexpected)`。`len=8252` 说明正文
  （含 `TAIL`）已完整收到，唯独 `closed` 事件缺失，`tcp_recv` 最终超时返回 -1。

* **根因（两层）**：

  1. 宿主转发器（`tcp_proxy.py` UDP 模式）不回串口通道那样 `SLIP_CAP=500 + 20ms` 节拍，而是
     `CHUNK=1392` 全速下行；8192B 响应经 `recv(4096)` 边界切成 **7×MSG\_DATA + 1×MSG\_CLOSED
     \= 8 个数据报**一次性涌入 guest；
  2. [netsock.c](../../v2-c-kernel/src/net/netsock.c) `netsock_drain` 用 `for(;;)` **全量排空 e1000 环**，
     把 8 个数据报一口气 `dispatch_frame` 进本地 socket 环（`NET_RXQ=8` 有效容量 7），第 8 个
     （恰为末位 `MSG_CLOSED`）在 `next==rx_head` 处被**队列满丢帧**；
  3. `MSG_DATA` 可牺牲（有 `ev_overflow` 观测），但 `MSG_CLOSED` 是控制事件绝不可丢——丢了即
     "数据到手、断连信号永远收不到"。

* **核实要点**：这是 BUG-047 的同款结构缺陷，只下移了一层——BUG-047 是 `tcp.c drain` 全量 pump
  挤爆 `TCP_RXB`（16KB 环）；本条是 `netsock_drain` 全量排空挤爆 `NET_RXQ`（7 槽小环）。SLIP 通道
  因其天然节拍（20ms 分块间隔 + 100Hz UART 轮询）逐帧消费而无突发，故未暴露；e1000 通道全速突发即现。

* **修复（已落地）**：`netsock_drain` 由"全量排空"改为**单帧泵取**（每次 `netif_rx` 只收一个 IP
  数据报即返回，跳过非 IP 帧）：突发缓冲交还给 **e1000 环（256 槽）**，socket 小环深度恒 ≤1，
  永不因全量排空丢尾。与 BUG-047 的"生产与消费同步/背压"思路一致。

* **回归（已补）**：test-tcp e1000 通道 `closed=1`、`tail=TAIL`、`refuse -1` 全绿；SLIP 通道不受影响。

* **教训**：任何"全量排空/全量 pump 进有限环"的地方都可能被更大突发击穿——修 BUG-047 时若同步
  审计 `netsock_drain` 这条同款路径，可少一轮复测；"控制事件"（CLOSED/OPENED/ERROR）与"数据"
  在 UDP 层同质，须保证承载它们的队列**绝不因数据突发而丢弃控制帧**。

***

## BUG-051 \[已修复] serial\_printf/serial\_puts 无 IRQ 原子性 → 并发日志行被撕裂（=K1，RR 判据失真总根因）

* **版本**：当前 main @ 9711cbc（RR 收尾期）· 独立测评 2026-09-03 定位

* **现象**：并发打印（shell 用户态经 syscall 打印 vs 定时器/调度内核打印）在**字符粒度**被打散，
  串口日志行被撕开。现场实录之一（rp\_torture golden `out.tr`）：
  `[shell] 'nosuchprog[sched] sleep pid=1 10 ticks (wake@466)' exited code=1`
  另一现场（edge 攻击会话）命令回显被切开：`n /u.c /u.[sched] wake pid=1 at tick=...`

* **根因**：[serial.c L56-L97](../../v2-c-kernel/src/drv/serial.c) `serial_puts`/`serial_printf` 逐字符
  `serial_putc`，**整行无 IRQ 原子性**——写一半可被定时器 IRQ 抢占，另一上下文再写即把行撕开。
  单 CPU 内核里"谁先写 UART"由中断抢占决定，非确定性。

* **影响（放大为 RR 阻塞项）**：① 串口取证/日志不可靠；② 撕裂行使 `func()`/契约正则漏匹配 →
  契约集不完整，**RR 的确定性/复原判据假红**（见 BUG-052）；③ 撕裂行污染基于 ack 计数的输入同步
  协议，使外部 agent/RR 录制握手失步。

* **关联**：BUG-042（F-4 selftest 行撕裂）是对 selftest 汇总行的**窄症状修复**，未根治本根因——
  其余任意行（shell 打印/命令回显）仍可撕裂。

* **修复（本次）**：给 `serial_printf`/`serial_puts` 整行加**状态保持的 IRQ 原子写**（`pushfl/cli`
  关中断写完再按原标志 `popfl` 恢复，绝不擅自开中断）。QEMU 串口 THR 几乎恒就绪，关窗仅几个
  指令/字符，icount 与确定性路径零影响。

* **回归（本次补）**：`test-tr` 复现性步连跑 5 次应 0 假红（修复前 \~67% flake）；重跑 rp\_torture
  A/D 段应无契约撕裂分歧；并发打印 stress 撕裂计数=0。

* **验证结论（fix 分支 2026-09-03）**：✅ 已落地并回归。`test-tr` 连跑 5 次 rc=0（0 假红）；
  见 BUG-052 fall 的"快照锚点"一并对尾流做固定。

***

## BUG-052 \[已修复] RR 复现性/确定性判据被撕裂行污染而假红（test-tr 复现性步 \~67% flake）

* **版本**：当前 main @ 9711cbc · 独立测评 2026-09-03

* **现象**：`test-tr` 第[4/4]步"复现性雏形"共 3 跑挂 2（~67%），症状为里程碑行 `[ls] /:` 前乱入
  回显字节被成 `l[ls] /:`（`ls` 首字符 `l` 被前置）；`rp_torture` A 段两轮契约分歧、D 段复原分歧，
  均指向"健康内核上自由度(golden)"自相矛盾。

* **根因（两层叠加）**：

  1. 底层：K1（BUG-051）——golden 捕获对"并发日志"本就非确定性撕裂；
  2. 判据层：[test\_transcript.sh 复现性步](../../v2-c-kernel/tests/test_transcript.sh) 与 rp\_torture 靠
     `pick()`/`func()` 的"行级干净输出"用全日志 grep，**未统一走** **`tr_mark ready`** **窗口归一化**
     （即交接单 F4 的"窗口锚"），boot auto-demo/命令回显混排即污染判据。

* **影响**：RR 在"golden/逐行契约"粒度是**不可信 oracle**——健康内核也会被翻成 "BUG?" 假红，
  降低回放差分的信噪比（误报淹没真报）。

* **修复（本次）**：① 根除 K1（BUG-051）后撕裂源消失；② 测试脚本判据统一改用 `tr_mark ready`
  锚 + `tr_window_after` 窗口化扫描（对齐 rp\_torture 与交接单 F4 指引），并对"单行被撕"做归一化兜底。

* **修复补充（本次，两根）**：

  1. `pick()` 判据：从"行级干净输出全量 grep"改为 `grep -aoE` **只抽里程碑子串**，
     隔绝不完整行残留字节污染 diff（对"行序"敏感、对"单行残留字节前缀"鲁棒）。
  2. `run_once` 快照锚点：原固定墙钟 `sleep 0.5` 会偶发吞掉末条命令（selftest）的尾部
     `[selftest] PASS` 里程碑（run-b 缺行 -> 假红）。改为**有界等待该里程碑落盘再快照**，
     超时不 fail-fast、交由 diff 如实反映真缺失。

* **验证结论（fix 分支 2026-09-03）**：✅ 落地后 `test-tr` 连跑 5 次 rc=0（0 假红）。

* **回归**：`test-tr` 连跑 5 次 0 假红；`rp_torture` A/D 段契约集稳定。

***

## BUG-053 \[已修复（使能）] 重型 QEMU 测试共享 build/ 目录 → 并发互踩（=R5，已 doc 已知）

* **版本**：当前 main @ 9711cbc · 独立测评 2026-09-03 复核

* **现象**：`build/transcripts`（TR\_BASE 默认）与各 harness 的 `TR_LOG` 均住在 `build/` 内；
  `make clean`（[Makefile L278](../../v2-c-kernel/Makefile)）`rm -rf $(BUILD)/`。任一并发重负荷 harness
  （test-tcp-attack 的 `make clean && make TCP_DEMO=1`、test-slip 等）都会**抹掉另一并发 harness
  的存活** **`build/transcripts/<runid>/`** **与串口日志现场** → 假 ack 超时 / 金标丢失 / 现场被删。

* **根因**：测试基建的构建/录制产物目录全局共享，无 per-harness 隔离；并发即竞态。

* **修复（本次，使能层）**：[Makefile](../../v2-c-kernel/Makefile) `BUILD` 由 `= build` 改为 `?= build`
  （可覆盖），并将 `CFLAGS -Ibuild`、`clean: rm -rf build tests/build` 统一改指 `$(BUILD)`。
  验证：`make BUILD=build-tmp` 产出 `build-tmp/kernel.elf`，`make BUILD=build-tmp clean` 完整清除。

* **已收口（全量隔离 wiring 完成）**：新增共享接线原语 `tests/_build_env.sh`——缺省 `BUILD=build`、
  自动 `mkdir -p $BUILD`；所有 QEMU/shell/宿主测试脚本在其 `cd ..` 后立即 `source` 该原语，字面
  `build/xxx` 一律改为 `$BUILD/xxx`。覆盖范围：16 个主脚本（qemu_regression / repro_bugs /
  rp_torture / replay / test_transcript / test_replay / test_net / test_socket / test_tcp /
  test_tcp_attack / test_tcp_dl / test_persist / test_determinism / test_serial / test_slip_net /
  test_cc500 / run_host_tests，经 grep 核实全部 source `tests/_build_env.sh`；transcript.sh 库经
  `${BUILD:-build}` 兜底）全量接入。
  使用方式：`make BUILD=<私有目录>` 或 `BUILD=<私有目录> bash tests/xxx.sh`；`TR_BASE` 缺省
  `${BUILD:-build}/transcripts`，随隔离目录各自独立。
  验证：`make BUILD=build-tmp` 产出 `build-tmp/kernel.elf`，produce/日志/fifo 全落 `build-tmp/`，
  `make BUILD=build-tmp clean` 完整清除且不触碰默认 `build/`；并发 harness 各用独立 BUILD 互不污染。

***

## BUG-054 \[已修复] 用户态传"区间内空洞页"给 syscall → 内核态缺页整机宕机（=外部评审）

* **版本**：当前 main · 修复于 PR #53（2026-09-03 合入）

* **现象**：用户程序把"用户半区 [0x80000000,0x81000000) 内**未映射空洞页**"指针传给任意
  涉用户指针 syscall（如 `sys_print(0x80500000)`），内核对态 `copyin_str` 读取该页触发页
  错误；缺页处理 [pf_handler](../../v2-c-kernel/src/mm/mem.c) 首分支判 `(r->cs & 3) == 3`
  （用户态）不成立（当前在内核态），又不落懒分配区 [0x40000000,0x41000000)，最终走
  `[FATAL] page fault … cli; hlt` **整机停机**。单个用户进程即可对整个内核 DoS——直接违背
  "agent 演练场"主线中"copyin/copyout 是敢跑任意编译产物时的安全前提"这一核心假设。

* **根因**：syscall 边界校验 `user_ptr_valid`（[userptr.c](../../v2-c-kernel/src/kernel/userptr.c)）
  只做**区间/回绕**校验 `[USER_SPACE_BASE, USER_SPACE_END)`，**不校验页是否已映射**；而用户
  半区绝大多数页从未映射（仅 app 槽 0x800A0000、共享 0x801A0000、堆 0x801A4000 及栈槽少量页
  被按需映射）。`copyin`/`copyout`/`copyin_str` 在内核态直接解引用用户指针，未映射页访问触发
  内核对态缺页，而缺页处理没有任何"内核态访问用户半区 → 隔离"的自愈分支 → 整机 [FATAL]。

* **排查过程**（既有防线为何没抓到）：[abuse.c](../../v2-c-kernel/src/app/abuse.c) 只测了内核
  低地址（0x100000）与 4GB 回绕（0xFFFFFFF0），恰好绕开"区间内空洞"；宿主 test_userptr 只验
  纯区间逻辑、不涉页表。QEMU 复现：`sys_print(0x80500000)` → `[FATAL] page fault @80500000
  err=0`（err P 位=0 确为未映射页；eip 为内核态地址）。

* **修复**（两轮，最终收敛到单一检查点）：逐页 `is_mapped()` 预检上移到
  `user_ptr_valid()` —— 区间/回绕判定通过后再逐页确认已映射，未映射即返 0，syscall
  在触碰前即 -1 返回：

  * 第一轮（PR #53）只在 `copyin`/`copyout`/`copyin_str` 三个 helper 上预检，**漏掉** 6 处
    仅调 `user_ptr_valid`(纯区间)就直接解引用的 syscall：`fs_write(15)`/`fs_read(16)`/
    `readline(20)`/`wait status(22)`/`exec argv(25)`/`recvfrom iov.buf(32)`。"fs 的 iov
    走 copyin 一并封堵"对 fs **不成立**——`fs_write(fd, 空洞,8)`、`fs_read(fd, 空洞,8)`、
    `fs_write(fd, 合法buf, 32768)` 三条仍会整机 [FATAL]（外部 A/B 的 V5/V6/V7 反证）。
  * 本轮把映射预检并入 `user_ptr_valid()` 单一收敛点，上述 6 处直接解引用 syscall 与
    `copyin`/`copyout` 全部受益（fs_write/read/exec 均在切页表/阻塞前于当前地址空间预检，
    无 TOCTOU）；`copyin`/`copyout` 删除重复的本地逐页 helper；`copyin_str` 保留跨页检查
    （`last_pg` 初值 -1 使首字节即查起始页，正确覆盖非页对齐起点）。
  * 宿主单测 [test_userptr.c](../../v2-c-kernel/tests/test_userptr.c) 的 `is_mapped` stub
    升级为"假页表"（仅低区 + 末页映射），新增空洞页/跨页界/末页拒绝断言，直接覆盖新预检路径。

* **回归**：QEMU 实测修复后 `sys_print(0x80500000)` 返 -1、abuse 全部边界用例 `(rejected)`
  + `[abuse] verify OK`、进程正常退出、整机存活；`make test-host` pass=20 fail=0；
  [abuse.c](../../v2-c-kernel/src/app/abuse.c) 新增门禁用例：`print@0x80500000`、
  `write buf@hole 0x80500000`、`read buf@hole 0x80500000`、`write buf@valid huge len=32768`
  （后三条对应 V5/V6/V7，任何一条再漏都会整机 [FATAL] 使 verify OK 不到），被
  qemu\_regression / test\_serial / rp\_torture 三门禁自动覆盖（一旦回归即整机宕、verify OK
  不到 → CI 判红）。

***

## BUG-055 \[已修复] 内核态命中用户半区缺页 → cli;hlt 整机停（第四道防线的死角）

* **版本**：当前 main（对 BUG-054 的纵深收口，加固而非新发缺陷的线上复现）

* **现象**：pf_handler 在**内核态（CPL=0）**访问**用户半区 [0x80000000,0x81000000) 未映射页**时，
  不落入 CPL=3 分支（用户越界隔离）、也不落懒分配区，直接走末尾 `[FATAL] … cli;hlt` **整机停机**。
  这不是正常用户路径（真正的用户越界被 CPL=3 分支隔离），而是"某条 syscall 漏掉 `user_ptr_valid`
  换算盲区"的唯一兜底路径——一旦未来新增 syscall 忘记收敛，一条命令即可整机宕机，违背
  "最坏只能杀进程"的安全铁律。

* **根因**：BUG-054 修的是**入口预检**（`user_ptr_valid` 单一收敛点），但 pf_handler 这条**最后防线**
  **仍以"整机停机"兜底**。入口层修得再全，缺一层"内核态访问用户半区 → 降级"的自愈分支。

* **实测复现**：临时跳过 `fs_write` 的 `user_ptr_valid`（模拟"未来 syscall 忘记收敛"），
  `fs_write(fd, 0x80500000, 8)` → `[FATAL] page fault @80500000 err=0` → 串口停止，整机停。

* **修复**：pf_handler 在 CPL=0 分支、落 `[FATAL]` 之前，新增"内核态命中用户半区"判定——
  命中则 `sched_kill(current)` 杀掉当前进程并切走**（降级，不再停整机）**，与 CPL=3 越界路径同构。
  - 不影响栈按需生长：STACK\_GROWTH 仅在 CPL=3 分支先行处理，本分支在 CPL=0、其后执行。
  - 不影响懒分配：懒区为低地址 0x40000000，不在用户半区判定内。
  - 仅此"用户半区"例外降级；其余内核 bug 级缺页仍 `[FATAL]` 停机以便诊断。

* **回归**：
  - 真实修复下 abuse 全门禁（含新增 C1 `print@MMIO 0xFEB00000`、C2 `write deepcopy len=65536`）全
    `(rejected)` + `[abuse] verify OK`，`make test-host` pass=20 fail=0、`make test-qemu` 全量通过。
  - 降级自验：再临时跳过 fs\_write 预检 → 改出 `[kern] PF user-half @80500000 … -> kill pid=5`，
    `FATAL=0`，tick 心跳持续（整机存活）。

***

## BUG-056 \[已修复] ELF 段 p_memsz 无上限 + 内核低 16MB 恒等映射假设 → 整机宕机隐患

* **版本**：当前 main（DoS 审计发现，非线上复现）

* **现象/隐患**：ELF 加载器对 `PT_LOAD` 的 `p_memsz`（段内存尺寸）**没有上限**，且与文件大小脱钩
  ——一个仅 84 字节的 ELF 可声称 `p_memsz=96MB`。`load_elf_file` 据此把 `load_region`（=ELF 自带
  区间）设为 96MB，`mapfn` 单次申请巨大映射：耗尽物理帧后，新的**页表帧**（`frame_alloc` 分配、
  [mem.c:158](file:///workspace/mini-os/v2-c-kernel/src/mm/mem.c#L158) 直接按物理地址当虚拟地址解引用）
  落在 >16MB 的恒等映射区外，内核态写未映射物理 → 非用户半区 [FATAL] 整机停（PR #59 的降级只管
  用户半区，兜不住此路径）。实测中因堆碎片 `kmalloc(记账数组)` 提前失败而常被掩盖为 -1，但属
  **状态/内存布局相关的整机宕机潜伏向量**，直接威胁"最坏只能杀进程"铁律。

* **根因**：① `p_memsz` 与文件大小脱钩、加载区间无用户空间/容量钳制；② `elf_load` 在 `mapfn`
  映射失败（`load_failed`）后仍执行 `memset_v` 清 bss，触碰未映射区；③ 内核仅为低 16MB 做恒等
  映射，却对**任意** `frame_alloc` 结果按虚拟地址解引用（>16MB 即失效）。

* **修复**（返回 -1 式降级，不开新 cli;hlt、不改 guard.c）：
  * [load_elf_file](file:///workspace/mini-os/v2-c-kernel/src/kernel/usermode.c#L124-L128)：
    `elf_load_range` 后强制 `lbase>=USER_SPACE_BASE && lend<=USER_SPACE_END && lend-lbase<=APP_ELF_MAXSIZE(1MB)`
    否则 -1 —— 把畸形 p_memsz 在映射前钳死；
  * [elf.c](file:///workspace/mini-os/v2-c-kernel/src/kernel/elf.c#L93-L94)：`elf_map_fn` 改返回 int，
    `mapfn` 失败即中止、不再 memcpy/memset 未映射区；
  * `app_mapfn` 恒返回 0/非 0（失败即广传）;
  * [heap.c](file:///workspace/mini-os/v2-c-kernel/src/mm/heap.c#L84)：`kmalloc` 补页计数把 16B 块头
   计入（`need=(size+HDR+4095)/4096`），修"请求恰为 N×4096 时恒分配失败"的功能缺陷（本审计的 P1）。
  * 残余（P2，未随本 PR）：根治 >16MB 解引用假设——为 pd/pt 等元数据帧保留低 16MB 专用帧池。

* **回归**：
  - 新增 DoS 夹具 `zbig`（84B 畸形 ELF，p\_memsz=96MB）进 initramfs；qemu\_regression / test\_serial
    `run zbig` 门禁断言"cannot load 'zbig'"（必 -1）且整机不 [FATAL]；
  - test\_elf 新增 4.11：mapfn 映射失败时 `elf_load` 必须中止且不写目标区/bss；
  - `make test-host` pass=20 fail=0（test\_elf 39 断言）；`make test-qemu` 全量通过（含 bigdemo 70KB、
    cc500 自举、shell 等合法加载不误伤）；`test-serial` zbig 被 -1 拒绝。

***

## BUG-059 \[已修复] 用户态 CPU 异常（#UD/#DE/#GP 等）→ 整机停机 DoS（SEC-01）

* **版本**：当前 main（2026-09-04 独立系统安全评估 SEC-01 实测发现）

* **现象/隐患**：`isr_handler` 默认分支对所有非 #PF（异常 14）/非 int 0x80（128）的 CPU 异常
  一律打印后 `cli;hlt`，且**不区分来源 CPL**。ring3 用户程序执行 `ud2`（#UD=异常 6）、除零（#DE）、
  `int3`/`cli`/非法段访问（#GP）等，均触发同一分支 → **整机永久停机**。实测：给 app 注入 `ud2` 后
  QEMU 日志 `[KERNEL] CPU exception #6 err=0 eip=800a0092`，此后调度/串口/网络心跳/键盘全部停止，
  必须重启 QEMU。低权限、一次触发、无前置条件——破坏"最坏只能杀进程"的隔离铁律。

* **根因**：缺与 `pf_handler`（用户态缺页 → 杀进程，[mem.c](../../v2-c-kernel/src/mm/mem.c#L247)）
  同构的"非 #PF 用户态异常杀进程"路径。异常入口桩（isr.s）已把用户帧切到内核栈（TSS esp0）、
  `registers_t` 含完整 cs/eip，分发层只需按 `r->cs & 3` 区分来源即可，无需改汇编。

* **修复**（[idt.c](../../v2-c-kernel/src/arch/idt.c#L98-L119)）：CPU 异常分支先打现场，若
  `(r->cs & 3) == 3`（来自用户态），打印 `[user] CPU EXCEPTION #n pid=k -> killed` 后
  `sched_kill(r, -1)`（非返回，内部 terminate_current 重调度）；仅内核态异常保留 `cli;hlt` 停机。

* **回归**：QEMU 实测 `run crash`（注入 ud2）：日志依次 `[KERNEL] CPU exception #6 eip=... ->
  [user] CPU EXCEPTION #6 pid=3 -> killed -> [sched] kill/reap pid=3 -> [shell] 继续响应 help`，
  系统存活继续运行（修复前此处冻结）；`make test-host` pass=20/fail=0、`make` -Werror 构建干净。

***

## BUG-060 \[已修复] FS 释放路径数据块号无界校验 → 恶意镜像越界写数据块位图（SEC-03）

* **版本**：当前 main（2026-09-04 独立系统安全评估 SEC-03）

* **现象/隐患**：`free_inode_blocks` 把 inode 的 `blocks[]/indirect/间接指针` 直接当作
  **数据块位图的位索引**（`bit>>3`）写入，无「块号落在数据区 `[FS_DATA_START, bd->blocks)`」校验。
  若 inode 表被篡改（挂载恶意 `-hda` 镜像，模拟冷启动外部输入的磁盘）──本内核 syscall 面写不到
  inode 表块（块 3），正常用户进程不可达，但攻击面=「未校验的冷启动磁盘镜像」──非法块号越界写位图
  块 2 相邻内存，可能破坏 FS 元数据或邻近内核数据。

* **根因**：分配侧 `alloc_block` 校验了 `bd->blocks` 上限，释放侧未做同构校验。

* **修复**（[fs.c](../../v2-c-kernel/src/fs/fs.c#L297-L319)）：新增 `blk_valid` 判定
  `blk>=FS_DATA_START && blk<bd->blocks`；`free_inode_blocks` 对直接块、间接指针、间接块本身
  释放前一律过校验，非法块号跳过（不动位图）。

* **回归**：`make test-host` pass=20/20（含 test_fs 正常释放路径不误伤）。

***

## BUG-061 \[已修复] 超长路径分量静默截断后继续解析（SEC-04）

* **版本**：当前 main（2026-09-04 独立系统安全评估 SEC-04）

* **现象/隐患**：`fs_walk` 分量缓冲 24B、截断到 23 字符后，剩余字符被**误当后续路径分量**继续解析
  ——「超长名不存在」被错解成「23 字符前缀对象 + 子路径」，`fs_create/fs_delete/fs_lookup` 语义错乱
  （如删错对象）。无越界写（缓冲有界），属解析语义缺陷。

* **根因**：分量读入循环 `cl < FS_MAX_NAME-1` 截止后未检测「分量未完」（`*p` 仍非 `/`）。

* **修复**（[fs.c](../../v2-c-kernel/src/fs/fs.c#L163-L168)）：分量读满 23 字符且后续仍是非 `/`
  字符时直接 `return -1`（拒判），不再吞剩余字符续解析。

* **回归**：`make test-host` pass=20/20。

***

## BUG-062 \[已修复] fs_make 忽略父目录只读，目录级 RO 可被绕建（SEC-05）

* **版本**：当前 main（2026-09-04 独立系统安全评估 SEC-05）

* **现象/隐患**：BUG-057 只读保护作用在**文件**层（`fs_delete/fs_rmdir/fs_write` 权威拦截），但
  `fs_make`/`dir_add` **不检查父目录的 `FS_MODE_RO`**，可在只读目录下新建条目（`fs_create/fs_mkdir`）。
  当前 initramfs 只 protect 文件不 protect 目录，故现状无实际绕过；若未来把目录设只读预期可被绕。

* **根因**：`dir_add` 与 `fs_make` 只在文件层查 mode，未在父目录维度对齐。

* **修复**（[fs.c](../../v2-c-kernel/src/fs/fs.c#L258)）：`fs_make` 分配 inode 前检查
  `fs_is_ro(bd, dir)==1` 即 `return -1`，与文件级语义对齐。

* **回归**：`make test-host` pass=20/20。

***

## BUG-063 \[已修复] DHCP 续约链内核栈写穿（4KB 栈叠加 5.5KB 缓冲帧）SEC-07

* **版本**：当前 main（2026-09-04 独立系统安全评估 SEC-07，`-fstack-usage` 实测）
* **危险度**：高 —— 静默内存破坏，无崩溃即越界写，破坏同栈数据。
* **漏洞链**：DHCP 租期续约由 `timer_cb` 在 **IRQ0 定时器中断上下文**驱动，运行在当前进程
  4KB 内核栈上（TSS `esp0` = 每进程 `kstack_top`，见 [sched.c](../../v2-c-kernel/src/kernel/sched.c) `KSTACK_SIZE`）。
  接收链路**逐层开着整帧/整报临时缓冲**，一帧在三级函数里被拷贝 3 份同时挂栈：

* **调用链 & 栈帧（修复前）**

  | 函数                            | 栈帧   | 缓冲                          |
  | ----------------------------- | ---- | --------------------------- |
  | `e1000_dhcp_tick` → `dhcp_poll_once` | 2112B | `bootp[NET_RXMAX=2048]`       |
  | `netsock_dhcp_recv`           | 48B   | -                           |
  | `netsock_drain`               | 1712B | `f[1600]`                    |
  | `netif_rx`                    | 8B    | -                           |
  | `e1000_if_rx`                 | 1648B | `eth[1600]`                  |
  | **合计**                        | **5528B** | **> 4096B → 写穿内核栈**       |

* **根因**：① `dhcp_poll_once` 用 `NET_RXMAX(2048)` 当 DHCP 报缓冲，而 DHCP 报文上限只是
  RFC 2131 最小重组缓冲 576B；② `e1000_if_rx` 先把整帧拷进本地 `eth[1600]` 再拷出，与上层
  `netsock_drain` 的 `f[1600]` 及 `dhcp_poll_once` 的 `bootp` 同栈叠加；③ 各缓冲均按"宽松上限"
  放大到 1600/2048，而实际一帧至多 1518B。
* **修复**（[netif.h](../../v2-c-kernel/src/net/netif.h)、[netsock.c](../../v2-c-kernel/src/net/netsock.c)、
  [e1000_netif.c](../../v2-c-kernel/src/drv/e1000_netif.c)、[e1000.c](../../v2-c-kernel/src/drv/e1000.c)）：
  - 引入 `NET_ETH_FRAME_MAX=1518`（恰容一帧，含链路头），`netsock_drain` 的 `f` 由 1600→1518。
  - `e1000_if_rx` 去掉本地 `eth[1600]` 中转：直接把整帧读入调用方缓冲后**原位剥头**，
    消除一帧的重复挂栈（1648B→64B）。
  - `dhcp_poll_once` 的 `bootp` 由 `NET_RXMAX(2048)`→576B（恰容一条 DHCP 应答），不再放大。
* **修复后栈帧**：`e1000_dhcp_tick` 64 + `dhcp_poll_once` 640 + `netsock_dhcp_recv` 48 +
  `netsock_drain` 1632 + `netif_rx` 8 + `e1000_if_rx` 64 = **2456B**（约 60% 内核栈）。
* **回归**（[check_stack_budget.sh](../../v2-c-kernel/tests/check_stack_budget.sh)）：新增
  `make test-stack` 门禁，用 `-fstack-usage` 逐帧断言链总和 ≤3584B（KSTACK 4096 − 512 裕量），
  并进了 `make test` 主链，秒级。`test-net` / `test-tcp`（e1000 UDP + 串口 SLIP）均 PASS，
  `test-host` pass=20/20。

## BUG-064 \[已修复] 内核栈预算总账（L0 页底 canary + L1 多根链门禁）SEC-07 延伸

* **版本**：当前 main（2026-09-04，承接 SEC-07/PR#70）
* **危险度**：中 —— SEC-07 只人工登记了 DHCP 一条链，其余 IRQ/syscall 入口无栈预算守护，
  未来任何新增深链（新网卡驱动、新轮询、深 syscall）都可能再次静默写穿 4KB 内核栈。
* **前提（单链独占栈）**：mini-os 所有中断/异常/syscall 均走中断门（进入即关 IF），故 4KB
  内核栈（`KSTACK_SIZE`，TSS `esp0`=每进程 `kstack_top`）同一时刻只承载单条处理链（IRQ 链 或
  syscall 链），**无嵌套叠加**。据此，"链栈帧总和 ≤ 预算"是贴切且安全的建模口径。
* **修复（双层防线）**：

  **L0 运行期兜底——内核栈页底 canary**（[sched.c](../../v2-c-kernel/src/kernel/sched.c)、
  [idt.c](../../v2-c-kernel/src/arch/idt.c)）：每个进程创建 kstack 后在页底
  `kstack_frame` 起 16B 写固定 magic `0x0C51E4D`（`kstack_arm`，覆盖 idle/spawn/spawn_at/fork
  全部 4 处创建点）；`isr_handler` 入口先 `kstack_check()` 校验当前进程页底 canary，被踩即打印
  `pid/esp/esp0` 并 `cli;hlt` 明确停机——把 SEC-07 的"静默内存破坏"变为"可诊断崩溃"。引导早期
  （`sched_init` 前定时器已 tick、栈未装填）`kstack_frame==0` 时跳过校验，避免误判。
  验收：临时注入 5KB 栈上缓冲探针，QEMU 运行日志出现 `[STACK-GUARD] canary stomped:
  pid=1 esp=29dbf0 esp0=29f000` 并停机；还原后回归全绿、无误报。

  **L1 静态防线——`test-stack` 扩展为关键根链清单**
  （[check_stack_budget.sh](../../v2-c-kernel/tests/check_stack_budget.sh)、
  [Makefile](../../v2-c-kernel/Makefile)）：把 SEC-07 的单链模型推广为多根独立断言（沿用
  聚合 constprop 取最大帧 + 单帧上限 3072B + 链总和 ≤3584B 模式），现守护 9 条根链：
  `IRQ0/timer·DHCP续约`(2472B)、`IRQ0/timer·调度`(80B)、`IRQ1/键盘`(80B)、`IRQ4/串口`(32B)、
  `syscall/recvfrom`(2072B)、`syscall/sendto`(3456B)、`syscall/exec`(1216B)、`syscall/fork`(448B)、
  `syscall/ls`(2528B)。
* **同批实现的栈帧收敛**（[usermode.c](../../v2-c-kernel/src/kernel/usermode.c)）：`test-stack`
  扩展后暴露出 `syscall_dispatch` 单帧达 **2224B**——`switch` 按最大 case 预留帧，`ls` 的
  `ents[FS_MAX_INODES]`(64×32=2048B)、sendto 的 `pbuf[1400]`、exec 的 `names[8][64](512B)`
  全挤在一帧；叠加 `netsock_send`(1648B)/`netsock_drain`(1632B) 深链会逼近乃至越过 3584B。
  把 3 个大缓冲 case 下沉为 `__attribute__((noinline))` 辅助函数 `sys_fs_ls_case` /
  `sys_exec_case` / `sys_sendto_case`（防 `-O2` 内联回 switch），`syscall_dispatch` 单帧
  降至 336B，各深链互不叠加。**预算口径不放松**（仍 3584B）——是先削中间层、不是放宽。
* **回归**：`make test-stack`（9 根链）绿；`make test-host` 20/20；`make test-qemu` 全绿；
  `make` -Werror 干净。L0 canary 探针验证检出并停机，还原后无告警/无误报。
* **后续（L2，本文档记录不作现版本实现）**：自动调用图栈预算工具——Python 读 `-fstack-usage`
  产出 + `-fdump-ipa-cgraph` 调用图，从全部 isr/irq/syscall 入口 DFS 求最长栈路径并断言 ≤ 预算；
  需登记函数指针边的白名单（`irq_handlers[]`/netif_ops/kb、serial hook），并对递归/环（sched
  不返回路径、idle 循环、cc500 递归）剪枝；为过渡期人工清单兜底，不追求"数学证明"。

***

## BUG-065 \[已修复] 内核按名/按路径加载缓冲(16B)与 FS 契约(FS_MAX_NAME=24)不一致 → 长合法程序名被静默截断撞前名前缀、误加载错误程序（Red Team F1）

* **版本**：当前 main（2026-09-04，红队审计 F1）
* **危险度**：高 —— 被截断的名字在前 16B 处斩断，`/hello` 与 `/hello_evil.bin` 之类会"共享前缀"；
  调用方本意加载 `/hello_evil.bin`，内核却静默以截断值 `/hello_evi` 去 FS 解析。
  若存在同前缀合法程序即**加载错误程序**；即便不存在也会报"找不到"而非"名字非法"，
  属静默截断型缺陷（CWE-130/Arm: 缓冲区截断后继续处理）。
* **根因**：`usermode.c` 三处按名/按路径加载入口（`usermode_spawn_elf` 内部 name 缓冲、
  `sys_exec_case` 的 `namebuf`、`case 21 sys_spawn_file` 的 `namebuf`）都用了 `char namebuf[16]`；
  而 FS 契约为 `FS_MAX_NAME=24`（单分量含 NUL，[fs.h](../../v2-c-kernel/src/fs/fs.h)）。
  于是 16~23 字节的合法程序名落入缓冲即被 `copyin_str`（静默截断）斩到 15B，随后以错误名解析。
* **修复**：
  * 三处缓冲统一扩至 `char namebuf[64]`（=path 约定，与 `case 13/14/18` 的 `char path[64]`
    一致；[usermode.c](../../v2-c-kernel/src/kernel/usermode.c)）。FS_MAX_NAME 只约束**单分量**，
    加载接口接受可含 `/` 的路径，故缓冲须 ≥ 路径上限而非仅单分量上限。
  * 新增 [copyin_str_full](../../v2-c-kernel/src/kernel/userptr.c)：与 `copyin_str` 相同但不静默
    截断——在 max 字节内未读到 NUL 即 `return -2`（超长），dest 已安全终止 `kern_dst[max-1]=0`。
    `sys_exec_case` / `case 21` 改用它，超长时显式 `-1` 并日志 `spawn_file name too long/invalid`，
    **不再静默截断撞前缀**。
  * [sched.h](../../v2-c-kernel/src/kernel/sched.h) 进程名显示缓冲 `pcb.name_buf` 同步 16→64B，
    消除"进程名显示被早截断"的隐性契约（仅观测用，与加载/FS 语义一致化）。
* **回归**：
  * host：test_userptr 新增 `copyin_str_full` 三态（成功 0 / 无效或未映射 -1 / 超长 -2）断言，
    并 mmap 出"假页表已映射低区"以真读成功/超长/跨页路径，36 断言全绿。
  * serial：新增"20 字符源名 + 18 字符加载名"正向用例（真实 `[elf] '...' loaded` + 运行输出 +
    `PASS (run>0)`），以及"超长/不存在"失败必须**显式报错、绝不输出假 PASS"负向用例。

## BUG-066 \[已修复] cmd_ccrun 用 uint32_t 收 sys_spawn_file 失败返回值 → -1 伪装成"PASS (run=0ms)"假成功（Red Team F2）

* **版本**：当前 main（2026-09-04，红队审计 F2）
* **危险度**：中 —— 打编译产物失败（spawn 返回 -1）时，shell 不报错、反而打印
  `[ccrun] '<out>' exited code=0 PASS (compile=…ms run=0ms)` 的假成功；`run=0ms` 即"根本没运行"
  的铁证被当成功吞掉，功能/教学语义失真（CWE-253: 返回状态作废）。
* **根因**：[shell.c](../../v2-c-kernel/src/app/shell.c) `cmd_ccrun` 用 `uint32_t pid = sys_spawn_file(tok[1])`
  接收返回值。`sys_spawn_file` 失败返回 `(uint32_t)-1`，赋值给无符号后 `pid<=0` 判断**恒假**，
  于是静默落入 `sys_wait(4294967295,...)` → 没等到子进程、`code` 保持 0 → 打印假 PASS。
* **修复**：运行段改用有符号 `int spid = sys_spawn_file(tok[1])`，`if (spid <= 0)` 正确判败并
  打印 `[ccrun] cannot run '<out>'` 返回；与 `cmd_run` 判败同构。编译段（fork）保持 uint32_t 不动。
* **回归**：test_serial 负向用例——对不存在的源码 `ccrun` 必须显式 `compile FAIL`，且"命令窗口内"不得
  再出现 `exited code=0 PASS` 行（按日志行号切片断言），杜绝 `run=0ms` 假阳性归来。

***

## BUG-067 \[已修复] sys_readline(max=0) 被当"未指定"替换为默认容量 → 向零容量缓冲写整行（Red Team 二轮 G1）

* **版本**：当前 main（2026-09-04，红队二轮审计 G1，RBT-2026-013）
* **危险度**：中 —— `sys_readline(buf, max)` 的 max=0 契约是"零容量"，但 [usermode.c](../../v2-c-kernel/src/kernel/usermode.c) case 20
  旧写 `uint32_t max = b ? b : KB_LINE_MAX+1` 把 0 吞成 129，会向"只给了 0 容量"的调用方缓冲整行写入
  （≤128B），违反调用方契约、成越界写引信（CWE-787 前兆）。
* **根因**：0 被误当作"未指定"哨兵。调用方必须给真实缓冲容量；0 本身是非法参数，而非"让我猜个容量"。
* **修复**：在 `user_ptr_valid` 之前加 `if (b == 0) { r->eax = (uint32_t)-1; return; }`——max=0 直接显式失败
  （返回 -1），尽早暴露非法调用方，而不是静默越界写。既有调用方（shell/echo 等 readline 均传 max≥1）无行为回归。
* **回归**：test_serial 新增用例——`sys_readline(合法指针, 0)` 不发任何键盘输入即返 -1 且不阻塞；旧实现会阻塞在
  `kb_line_ready` 等输入而永不返回，断言超时判败。载荷 cc500 方言合规（`0-1` 表达式规避 unary 负号、无数组/for/break/cast）。

***

## BUG-068 \[已修复] 删除文件后仍打开的 fd 持 inode 号，inode 最低位复用 → 旧 fd 写落入新文件（Red Team 二轮 D4）

* **版本**：当前 main（2026-09-04，红队二轮审计 D4，RBT-2026-014）
* **危险度**：高 —— 删除文件后旧 fd 仍持该 inode 号；`alloc_inode` 最低位复用该 inode 给新对象后，旧 fd 的写会
  **静默落入"新文件"**（跨文件写、无告警、数据混淆）。fs 层是纯逻辑（宿主单测直接编译），回收不得引入 sched 依赖。
* **根因**：[usermode.c](../../v2-c-kernel/src/kernel/usermode.c) case 19/28 删除成功即释放 inode，但未回收仍指向它的
  进程 fd 槽；调度层 [sched.c](../../v2-c-kernel/src/kernel/sched.c) 无"按 inode 回收 fd"的接口。
* **修复（方案 A，回收走进程面/调度层，fs 层零改动）**：
  * [sched.c](../../v2-c-kernel/src/kernel/sched.c) 新增 `sched_fd_revoke(uint32_t inode)`：遍历 `procs[1..MAX_PROCS)`，
    对非 FREE 进程 fd_table 中 `used && fd.inode==inode` 的槽置 used=0，记日志 `[fs] revoke fd=%u inode=%u (pid=%u)`；
    [sched.h](../../v2-c-kernel/src/kernel/sched.h) 声明。
  * [usermode.c](../../v2-c-kernel/src/kernel/usermode.c) case 19（fs_delete）与 case 28（fs_rmdir）：fs 返回成功（0）后，
    以解析得到的实际 inode 调用 `sched_fd_revoke(ino)`。回收后旧 fd 读写立即 -1，杜绝跨文件写。
* **回归**：test_serial 新增用例——create A→open fd=1(A,wr)→write OLD→rm A→create B→open fd=2(B,wr)→write BOK→
  经悬垂 fd1 写：断言出现 `[fs] revoke` 日志、fd1 写返 -1、且 `cat /dB` 仅含 BOK 绝无悬垂 fd 字节。

***

## BUG-069 \[已修复] 恶意镜像块号指向 inode 表/位图 → 文件读写别名击穿系统文件只读（Red Team 三轮 RD3-V2）

* **版本**：当前 main（2026-09-04，红队三轮 RD3，V2）
* **危险度**：高 —— fs 层把磁盘 inode 的 `blocks[]`/`indirect` 当块号直接寻址，不校验合法性。恶意镜像篡改某文件
  `blocks[0]=3`（inode 表区块号）后，该文件读写别名作用于 **inode 表**：据此可清掉受保护文件（cc500.c 等）的
  `FS_MODE_RO` 位，从而**击穿 BUG-057 的系统文件只读**（实测 `fs_is_ro 1→0`、受保护文件内容被改、静默无崩溃）。
* **根因**：`blk_valid`（要求 `blk∈[FS_DATA_START=4, bd->blocks)`）此前只被 `free_inode_blocks`（释放路径）调用，
  读/写/目录遍历这些"把 inode 块号用于寻址"的路径一律不校验。
* **修复**：`blk_valid` 推广到一切 inode 块号寻址路径——[fs.c](../../v2-c-kernel/src/fs/fs.c)
  `dir_add`/`dir_remove`/`dir_empty`/`fs_lookup_in`/`fs_list_dir` 对损坏块视同跳过（不解引用、不参与找槽）；
  `file_block` 统一语义「返回可用数据块号；损坏块号 == 0（不存在/不可用）」：直接块 / indirect / 拾取块号
  非 0 但 `!blk_valid` 一律返回 0；`create=1` 时损坏块号**不可重分配覆盖**（该块可能被镜像别处引用），返回 0
  走 `fs_write` 既有"首块失败 -1 / 中间块短写"逻辑，`fs_read` 既有 `blk==0 break` 在损坏点截断。块 0-3
  （位图 / inode 表）永不作为数据块寻址。
* **回归**：宿主单测（[test_fs.c](../../v2-c-kernel/tests/test_fs.c)）——建普通文件 A + protect 文件 P →
  篡改 blockdev inode 表令 A.blocks[0]=3（inode 表）→ `fs_read/write(A)` 拒绝（-1/0）、P 仍含 `FS_MODE_RO`、
  内容未变、ASan 清洁。

***

## BUG-070 \[已修复] 恶意镜像越界块号 → blockdev_ptr 返 NULL 被解引用（Red Team 三轮 RD3-V1）

* **版本**：当前 main（2026-09-04，红队三轮 RD3，V1）
* **危险度**：中 —— 块号越界时 `blockdev_ptr` 返回 NULL，fs.c 多处在解引用前不判空：
  `fs_lookup_in` 的 `e[k].inode`（宿主 ASan SEGV）、`file_block` 的 `ptrs[ib]`（ASan SEGV）；
  guest 端低 16MB 恒等映射使 NULL（页 0）写不触发 #PF → **静默破坏**（不崩溃但改错内存）。
* **修复**：
  1. **1a 校验推广**：把"损坏块号（越界 / 指向元数据）"在所有寻址路径统一前置 `blk_valid`，视同损坏跳过 / 返回 0
     （与 BUG-069 同一处 `blk_valid` 推广，二者共享根因、一同修复）。
  2. **1b 判空纵深**：上述 5 处 `blockdev_ptr` 返回后、解引用 `e` / `ptrs` 前各加 `if (!e) …` 保底
     （`blk_valid` 已挡越界，此处正常不可达，纯纵深防未来 NULL 路径）。
* **回归**：宿主单测——V1a 文件直接块=0xFFFFFFF0（read 截断、write 首块 -1、不崩）；V1b 间接块指针=越界
  （间接区 read 截断、write -1、不崩）；V1c 目录块=越界（`fs_lookup_in`/`fs_list_dir` 跳过损坏块安全返回）；
  均含恢复后"正常性对照"（校验不误伤合法块号）。

***

## BUG-071 \[已修复] 合法范围内重复块 → 跨文件读泄漏 / 写污染、击穿 BUG-057 系统文件只读（Red Team 四轮 RD5-V4）

* **版本**：当前 main（2026-09-04，红队四轮 RD5，V4）。属 RD3（BUG-069/070）"诚实边界"清单中的**内容层矛盾**。
* **危险度**：高 —— BUG-069/070 只对块号做**范围校验**（`blk_valid`：∈`[FS_DATA_START, blocks)`），挡不住
  **"合法范围内的重复块"**：恶意镜像把两个 inode 的 `blocks[i]`（或 `indirect`）指向**同一合法数据块**。
  后果（三式）：
  1. **跨文件读泄漏**：经 B 读"A 也指向的块"→ 读到 A 的文件内容；
  2. **跨文件写污染**：经 B 写该块 → 直接改写 A 文件内容；
  3. **击穿 BUG-057**：若该块是**受保护（`FS_MODE_RO`）文件 P 的数据块**，BUG-057 只查"写者自身 mode"
     （W 可写就放行），W 的写会原样落到由 P 拥有的数据块 → **静默改掉只读文件内容**（V4×RO）。
* **根因**：处 [fs.c `file_block`](../../v2-c-kernel/src/fs/fs.c) 只信"块号在范围内"即作为本文件数据块，不校验
  **块到底归哪个 inode 所有**。正常路径由 `alloc_block` 位图保证唯一，本不应冲突；只有恶意镜像（不经 alloc、
  在磁盘 inode 里直接写重复块号）才会出现。
* **修复（块归属账本）**：
  1. **账本**：`static uint16_t owner[BLOCK_SIZE*8]`（容量=数据块位图单块位容量，覆盖任意 ≤32768 块设备；
     RAMDISK=256 远小），记录"块 → owner inode"。**owner 值 = ino+1 编码**（0=空闲无主；1..64 映射 ino 0..63），
     因 `FS_ROOT_INODE=0`，裸 ino 会让"root 拥有块"(值 0) 与"空闲/无主"歧义、first-declarer-wins 在 root 上失效。
  2. **登记** `owner_claim`：新增块（`dir_add` 目录扩容、`file_block` 直接/间接/拾取块）alloc 后即登记，账本随
     分配同步无孤儿；同 ino 自引用不冲突，冲突保留首个声明者。
  3. **校验** `owner_check`：`file_block` 每次返回块号前校验 `owner[blk]==ino`——直接块、间接块本身、拾取块都查，
     **不符即返回 0**（读截断 / 写走"首块 -1 / 中间短写"，与 RD3 语义一致），不覆盖他人块、不打位图重复分配。
  4. **扫描重建** `fs_scan_owners`：挂载外部镜像（`storage.c` 持久盘命中 `FS_MAGIC`）后遍历在用 inode 全部
     直接/间接/拾取块登记；重复声明记 `fs_owner_violations`。挡住"恶意镜像不经 alloc 声明范围内重复"。
  5. **释放清账** `owner_clear`：`free_inode_blocks` 释放块即清归属，与位图回收同步。
  6. **可观测**：`fs_owner_violations_get()` 在 `kern_audit` 打印（仅观测、不入 bad 和——防守成功的"被阻断"
     不是健康失效，正常引导恒 0）；`fs_owner_orphans()` 报"位图在用但账本无主"的孤儿数。
* **回归**：宿主单测四式——① 健康基线：干净镜像 scan 后 0 冲突、0 孤儿（无假阳性）；② V4 跨文件：B 的块指向
  A 的合法数据块 → scan 检出冲突、经 B 读截断 0 / 写 -1、A 自身不受影响；③ V4×RO：可写 W 的块指向受保护 P 的
  块 → W 写被首块阻断、P 内容与只读位不变、不击穿 BUG-057；④ V4 间接块重复：Y 的 `indirect` 指向大文件 X 的
  间接块 → scan 检出冲突、经 Y 越直接区读/写被阻断（间接块本身须归本 inode）。

***

## 加固 A-1（BUG-072+）\[已修复，2026-09-05] 内核 SSP + panic 现场回溯 + ring3 chaos 探针（可观测性加固）

> 承接 #69（异常族单点已清）后的补位：非"某个崩点"缺陷，而是**编译期兜底 + 故障可观测性**两层加固，
> 降低"内核被改坏/栈被写穿时静默继续"的运维黑洞。

* **版本**：当前 main（2026-09-05，加固 A-1）
* **动机**：此前内核编译用 `-fno-stack-protector`，且 CPU 异常/panic 只有一行 `int_no`，无法定位调用栈；
  ring3 坏指令虽能被 SEC-01 隔离杀掉，但缺少系统性"随机指令流"探针来检验隔离不退化。
* **修复**：
  1. **① 内核 SSP**：CFLAGS 开 `-fstack-protector-strong -mstack-protector-guard=global -fno-omit-frame-pointer`；
     [ssp.c](../../v2-c-kernel/src/kernel/ssp.c) 用 RDTSC 混合随机化 `__stack_chk_guard`（区别于 app 层固定值，
     防针对性绕过），`ssp_seed()` 在 `kernel_main` 早期（serial_init 后）执行；[ssp.c#__stack_chk_fail](../../v2-c-kernel/src/kernel/ssp.c)
     金丝雀被改写时打印 `[PANIC] kernel stack canary mismatch pid/eip/esp` 后 `cli;hlt` 停机——宁要可诊断停机，
     不要静默内存破坏。
  2. **② panic 现场增强**：[ksym.c](../../v2-c-kernel/src/kernel/ksym.c) 提供符号表（`tools/gen_kernsym.sh` 经 `nm`
     提取 + 两阶段链接嵌入）`ksym_name` 二分 + `dump_backtrace`（EBP 链）+ `panic_dump`（寄存器 + eip 符号化 +
     内核态回溯 / ring3 注明）；接入 [idt.c](../../v2-c-kernel/src/arch/idt.c) `isr_handler`——用户态异常隔离杀该进程，
     内核态异常才停机。
  3. **④ ring3 chaos 探针**：[apps/chaos.c](../../v2-c-kernel/src/app/chaos.c) 每轮 fork 子进程随机执行
     ud2/int3/cli/hlt/div0/lgdt 一条，必触发 #UD/#BP/#GP/#DE，内核须隔离杀该子进程（panic_dump 现场 +
     sched_kill），父进程连跑 6 轮后自审计 0 存活；`run chaos` 接入 `test_serial.sh`（断言 survived 0 + 无 NOT_TRAPPED）。
* **回归**：`make`(-Werror)、`test-host`(pass=20 fail=0)、`test-stack`、`test-serial`、`test-qemu` 全绿；
  手动验证——内核态临时栈溢出触发 `[PANIC] kernel stack canary mismatch` 停机（非静默），
  chaos 各轮 #GP/#UD/#DE dump 寄存器 + ring3 隔离，6 轮后系统存活。
* **非目标**：不引入 PAE/NX、不做 ASLR、不变中断模型、不扩 KSTACK（按 A-1 非目标声明）。

***

## 威胁模型注记（RD3 诚实边界，及 RD5-V4 收口）

**RD3 断掉的链路**是**"元数据区被当数据读写"**：块号被篡改成指向位图 / inode 表 / 越界地址时，文件读写不再
跨区别名、不再解引用空指针。
**RD3 诚实边界 → RD5-V4 已收口**：RD3 曾声明"若恶意镜像把两个文件的 inode 直接指向**同一个合法数据块**（内容
层矛盾），两个文件仍会相互污染，需镜像校验工具承担"——该具体缺口现已由 **BUG-071 块归属账本**封闭：任何"非本
inode 所有"的数据块访问（含经它写 RO 文件块）在 `file_block` 显式失败。此处补记RD3 原边界条款的演进。
**RD5 诚实边界**：账本挡住**归属层**的范围内重复（块归谁的矛盾）；它不保证 inode 外其它**元数据自洽**（如目录
条目哈希失控、目录/文件同名 etc.），这些仍需按需走镜像一致性工具。账本本身非持久化（启动按镜像重扫），只读块
的"内容正确性"仍依赖写入方来源可信。

***

## 工程踩坑（非代码缺陷）

| 编号      | 场景               | 现象                                                                                                                   | 处置/教训                                                                                                                                             |
| ------- | ---------------- | -------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| OPS-001 | git 提交（沙箱环境）     | `Author identity unknown`：环境未配置 user.name/user.email                                                                 | 逐次用 `git -c user.name=… -c user.email=… commit` 指定，不改全局 config；`git log` 历史可溯源                                                                    |
| OPS-002 | 编辑 selftest 日志调用 | 改 ARP 自检日志时把 `serial_printf("… #%d", attempt)` 误改为 `serial_puts("… #%d")`                                            | `serial_puts` 单参、`%d` 只是普通字符 → **编译不报错、输出丢参数**；且现有断言 `selftest: tx ARP req` 不校验 `#N` 数字 → 回归抓不到。提交前人工核对日志格式参数；日志格式化参数丢失类问题应靠"打印变量值"的断言或 diff 日志发现 |
| OPS-003 | v0.26#3 堆区迁址     | 用户堆区由 0x800B0000 迁至 0x801A4000 后，`test_serial.sh`/`qemu_regression.sh` 的 heapdemo 断言仍写旧地址 0x800b0000 → 首轮回归 3 项 FAIL | **地址类断言与布局常量强耦合**：每次改动 mem.h 布局必须全仓搜索该地址（`800b0000`/`USER_HEAP_BASE`）同步测试。教训：测试里的"魔法地址"应尽量引用常量或用正则前缀（如 `brk=0x801a4000` 单独写成一行便于批量更新）             |
| OPS-004 | bigdemo 单行输出拆断断言 | `[bigdemo] 70KB write+verify sum=… survived big-ELF load` 两段打在同一行，断言 `\[bigdemo\] survived` 需"连续子串"，grep 匹配失败        | **grep 是连续子串匹配，不是"包含两个独立短语"**。多段语义日志应拆成独立行（本项目约定"每行单次 sys\_print 原子行"正是为此），断言也按行拆分；避免"一行塞两个可断言关键词"                                                |

## 未解决问题（观察记录）

| 编号                            | 现象                                                                                                                                                                                                                           | 结论                                                                                                                                                                                                                                      |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| OBS-001                       | 定时器中断内读 PIT 计数器常为 0                                                                                                                                                                                                          | 正常现象：中断发生在计数器回绕时，读到的瞬时值即 0，并非硬件故障                                                                                                                                                                                                       |
| OBS-002                       | ~~`pcb_t::fork_frames[24]`~~ ~~硬编码 24 帧（96KB）限制大进程 fork~~                                                                                                                                                                    | ✅ 已修复（v0.30 BUG-035）：改为 kmalloc 动态数组，`sched_fork` 按需分配、退出 kfree                                                                                                                                                                         |
| OBS-003（=评审残留）                | netsock send/recv 无进程归属（`netsock_sendto`/`recvfrom` 不校验调用者是否为该 socket 的打开者）                                                                                                                                                  | 现设计语义：单用户教学 OS 将网络 socket 视为进程共享资源。close 已做归属隔离（BUG-038），send/recv 保持共享；威胁模型声明之，需强化时再列缺陷                                                                                                                                                |
| OBS-004（=F-6）                 | `writefile` 整行 128B 截断（shell/kb 行缓冲容量）——超长源码被截断                                                                                                                                                                              | F-3 修复前是未闭合字符串崩溃引信；F-3 修复后仅为教学限制（单行 ≤128B）。**✅ 已缓解（v1.4）**：`writefile <<DELIM <path>` heredoc 多行写入，逐行收集直至独立 DELIM 行，任意行数拼接，绕开单行截断；每行仍 ≤128B、单位逻辑行不允超（键盘单行物理上限），大程序经 heredoc 一次写入。验证：test\_serial heredoc 用例（>128B 多行源码 ccrun 编译运行 PASS） |
| OBS-005（压测 2026-09-01）        | `qemu-i386` 直跑 cc500 编译产物 ELF 退不出真实退出码（产物假设 mini-os 整机 int 0x80 契约，无内核支持）——`int main(){int a;a=1+2;return a;}` 宿主直跑一律返 1，一度误报为"变量返回算错"                                                                                       | **语义验证必须走整机 ccboot 内核 ccrun**，宿主仅可信"编译 rc 0/1"（畸形 fuzz 3000 例即只测编译阶段）。整机证实 `a=1+2;return a` 实返 3，算法无误，非缺陷                                                                                                                               |
| OBS-006（压测 2026-09-01）        | ccrun 判定 `code==0 ? PASS : FAIL`（[shell.c](../../v2-c-kernel/src/app/shell.c) L473 非 0 一律 FAIL，`main{return 0}` 视为成功）                                                                                                              | 验证"某个正确的非零值"不能靠 `return 该值`（必被标 FAIL）；应程序内部 `if(x==期望)return 0;return 1;`。已据此把 test-cc500 guest 层扩为 `a=1+2;a==3` 整机真值断言，顺带覆盖 `==`（此前 guest 层只测 `<`）                                                                                     |
| OBS-007（虚拟 TCP 压测 2026-09-01） | 恶意下行事件（未知 sid 的 CLOSED/ERROR/TIMEOUT/OPENED + 非法 mtype + 错版本号 + 保留位非 0 + 超大 MSG\_DATA 1392B + 过短头 0..7B + 随机字节）**并行 10s × \~900pkt/s 注入**，合法 HTTP 8KB+TAIL 仍 `RESULT PASS closed=1 tail=TAIL refuse=-1`，内核无 OOP/PANIC/TRIPLE | 验证 BUG-050（netsock 单帧泵取背压 e1000 环）和会话解析器 `conn_by_session(sid)==NULL` 的"无副作用 drop"——4209 脏数据报没混进合法连接，也没撑爆 NET\_RXQ。固化为 `tests/test_tcp_attack.sh + tests/tcp_attack.py`，注入面=转发器 UDP 监听口（与 test-tcp 同入口，可 CI 直接跑）                        |
| OBS-008（协议 压测 2026-09-01）     | 宿主层解析器（IP / UDP / ICMP / DHCP / SLIP / 虚拟 TCP 会话头）**ASan+UBSan 150w 轮 fuzz = 1200w 次 parse 调用**，无崩溃/越界/UB（19 项宿主单测 0 fail）                                                                                                   | 解析器加固到位，1200w 调用未暴露新协议漏洞。ASan+UBSan 清洁是业界回归基线；保留 `tests/fuzz_parse.c` + `run_host_tests.sh`，`FUZZ_ITERS=1500000` CI 可直接跑。                                                                                                               |
| OBS-009（返回状态穷举 2026-09-03）   | 用"枚举每个分配/映射接口返回状态"审计 `frame_alloc`/`map_page_in` 调用点：`usermode.c app_mapfn` 加载 ELF 时 `map_page_in` **页表帧 OOM 后静默丢映射**（后续该 app 跑触该页才缺页崩）；懒分配 [mem.c](../../v2-c-kernel/src/mm/mem.c) `map_page` 丢弃 `-1`（可能 fault 重试循环）；sched/idle 初始化及 e1000 MMIO 映射未校验返回 | 静态推演的 **OOM 边界**（正常内存配置不可达，需帧池耗尽才触发）。`frame_alloc` 各数据帧调用点多已降级（BUG-033 后维护良好）；真缺口在 **usermode app_mapfn 页表帧 OOM → 未置 load_failed**（P2，~3 行：`if(map_page_in(...)!=0){ load_failed=1; return; }`）及 lazy/init/sched 未校验（P3）。建议按 P2 先收口 app_mapfn |
| OBS-010（ATA 设备残留）             | ATA 超时恢复：`ata_wait_ready` 超时返回 -1 但**未发 SRST 软复位**，设备命令状态可能残留（[ata.c](file:///workspace/mini-os/v2-c-kernel/src/drv/ata.c#L52-L61) 超时路径）——QEMU 下近不可达，仅 fs_sync/save 偶发触发且返回 -1 不崩                                                                                              | 低危观察；建议超时后发 `0x08(SRST)` 软复位恢复（状态机 + 超时计数，勿用 `sleep`）；当前未改，待后续小 PR                                                                                                        |
| OBS-011（独立安全评估 SEC-06，2026-09-04） | 缓解技术缺口：无 ASLR/PIE（用户程序固定链接 `0x800A0000` 等）；无 NX（用户代码/栈/堆均 `0x7` P|RW|U 含可执行映射）；ELF `e_entry` 不校验落用户可执行区 | **单用户教学模型下可接受的有意取舍（降级"代码注入+ROP"成本，但无提权路径）**；页表 U/S 位隔离是根本防线（实测有效）。建议在 `docs/security.md` 威胁模型声明"未启用 NX/ASLR，功能演示优先"（对应 CWE-693）；不改码 |
| OBS-012（独立安全评估 SEC-02 交叉引用，2026-09-04） | UDP socket 无进程归属：`netsock_send`（[netsock.c](file:///workspace/mini-os/v2-c-kernel/src/net/netsock.c#L93-L101)）/`netsock_recv`（[:103-116](file:///workspace/mini-os/v2-c-kernel/src/net/netsock.c#L103-L116)）只查 id/used、不校验 pid，任意 ring3 进程可对他人 socket 窃听/伪造/排空；仅 close 有归属检查（`netsock_close_if_owner`） | **= 既有 OBS-003**：作者已裁定为"单用户教学 OS 将 socket 视为进程共享资源"的设计取舍。独立评估接受其为有意设计；建议在 docs 显式写出该威胁模型，注明"未来存在互不信任 guest/agent 时须为 send/recv 增加归属校验"（校验成本低，见 BUG-038 同构写法） |
| OBS-R1（红队二轮，2026-09-04，RBT-2026-013 相关） | 串口键盘通道丢 ≥0x80 字节：kb 通道对单字节 ≥0x80 的输入长期/复用路径可能丢失或改写，未做过滤即入行缓冲 | 仅记录、不修，归 P1：当前 agent/命令均为纯 ASCII（≤0x7F），无功能影响；如需通道过滤改造或转义支持另立项 |
| OBS-R2（红队二轮，2026-09-04） | cc500 无数组/for/break/cast 文法，而 README/文档有"用局部数组"等表述，与编译器实际能力不符 | 仅记录、归 P1：文档表述与编译器能力需对齐（改文档或扩文法二选一），本轮不改；测试载荷一律写"无数组"方言 |
| A4（红队二轮，2026-09-04） | 存活父进程若迟迟不 start/复收，已退出子进程的僵尸会钉死 pid 槽，导致 MAX_PROCS 内新 spawn 取不到该 pid | 设计权衡，接受：单用户模型下父进程相位短、僵尸无 double-free；待父进程 sys_wait 复收即释放，无需改（P3 记录） |
| A5（红队二轮，2026-09-04） | sleep 传巨值时 ticks 判定回绕 → 被当作"已到点"立即唤醒，而非长眠 | 仅记录、不修：单用户无真实长时等待需求，回绕语义即"错误即醒"是安全侧的有利方向；若需真实延时另设计（P3 记录） |
| OBS-013（门禁 flaky 治理，2026-09-04，插曲 1/2） | 门禁间歇性红：**类型 I** 时序 flake（等断窗口不足→整行缺，如插曲 1 deepfork 退出码缺行、B test-socket F-0a wait_for 20s 超时、C test-qemu abuse 超时、D DHCP 时序级联；均在等断函数、零打点）；**类型 II** 断言/实现缺陷（行在场但格式不符，如插曲 2 test_serial 长名 `run=[1-9]` 在 10ms tick 边界误判 run=0ms 合法快执行） | 治理三层（Commit 1~4，ops 已批准 Commit 1）：① **打点**——三脚本 wait_for/wait_after 每断言恰打一行 `$BUILD/assert_timing_<script>.tsv`（断言名\t耗时ms\tok\|timeout，250ms 轮询粒度），FAIL/超时打 LOG 尾 ~20 行现场；随 CI artifact 上传，经 `tr2sqlite.py --assert-timing` 导入、`baseline_check.py --asserts` 跨轮 P50/P95 基线。② **分类口径**——**假失败必先看 FAIL 现场**，整行缺=类型 I（窗口/重试，留待 B2 打点数据后再议放宽 timeout），行在格式不符=类型 II（断言根修，如插曲 2 已收敛 `run=[0-9]+`）；禁止无分类归档。③ **断言名即 RR 基线 key**，改名即新基线，须保持稳定。目标：类型 I 靠数据积累后按 B2 流程提案，类型 II 当场根修；本轮不动 B2 20s 阈值、不新增 job/不改 job 名 |

