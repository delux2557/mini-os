# 架构设计与开发思路（Design）

## 1. 总览

Micro-OS 是 x86 32 位保护模式内核，通过 multiboot 协议由 QEMU 直接加载
（`-kernel` 方式，无需外部 bootloader）。所有源码在 `v2-c-kernel/src/`。

```
boot.s(multiboot 头 + 进入内核) → kernel_main
   ├─ idt_init()      IDT + 8259 PIC（IRQ 重映射 0x20~0x2F）
   ├─ mem_init()      物理内存检测（multiboot 信息）
   ├─ paging_init()   开启分页
   ├─ heap_init()     内核堆
   ├─ usermode_init() 重建 GDT（含 ring3 段）+ 加载 TSS
   ├─ timer_init()    PIT 100Hz 心跳
   ├─ kb_init()       键盘（v0.9：装行完成回调 → sched_wake_keyboard）
   ├─ serial_set_rx_hook(kb_feed_char)  v0.10：串口接收 → 键盘行缓冲（输入源统一）
   ├─ ramdisk_init()  v0.8 内存盘 + fs 格式化
   ├─ initramfs_setup() v0.9 把内嵌应用 ELF 写入 ramdisk（motd/hello/echo/crash/shell）
   └─ sched_init/spawn/start → 多进程调度接管（不返回）
       └─ usermode_spawn_elf("shell", ...)  v0.9 加载常驻 shell
```

## 2. 内存布局

| 区域 | 说明 |
|------|------|
| 0x100000 ~ 内核末尾 | 内核代码/数据（保护页） |
| 物理帧分配器 | 以 4KB 页为单位，位图/链表式分配 |
| 0x80000000 | 所有进程共享的用户代码页（映射同一份 userprog.bin） |
| 0x80010000 + pid*8K | 每进程用户栈 8KB 槽 = [守卫页 4KB(不映射) \| 栈页 4KB(映射)]（v0.13，栈从槽顶向下增长） |
| 0x80044000 + slot*4K | 共享内存页（v0.13 后移避让栈区；所有进程映射同一物理帧到同一虚拟地址） |
| 0x80030000 | 常驻 shell 的链接/加载地址（v0.9，resident 帧不随退出回收） |
| 0x80040000 | 应用槽（v0.9：`run <prog>` 加载的 ELF，退出即回收其代码帧） |
| 每进程 4KB 内核栈 | 中断切换用，TSS.esp0 指向其栈顶 |

- 懒分配：用户/测试页首次访问触发页错误，`pf_handler` 检查懒分配区并补映射，随后重试。
- 内核地址区写保护：ring3 越权写内核页 → 页错误 → 进程被终止（v0.4 起有 crash 演示）。
- v0.11 起：**每个用户进程持有独立页目录**（见第 9 节），不再共享页表；
  各进程用户半区（≥2GB）映射互不可见，内核半区（低 16MB 恒等映射 + 懒分配区）克隆共享。
- 共享内存机制（v0.6 设计，v0.11 适配）：物理共享帧只分配一次，但**每次调用 `sys_shmem`**
  都把该帧重新映射进当前进程页目录，保证各进程都能访问同一物理帧。

## 3. 中断与系统调用

- IDT：32 个 CPU 异常 + 16 个硬件 IRQ + `int 0x80` 系统调用门（DPL=3，允许用户态触发）。
- `isr_common_stub` 统一入口：压入寄存器现场 `registers_t`（gs..eax、中断号、eip/cs/eflags/user_esp/ss）。
- 系统调用分发 `syscall_dispatch`：`eax=号`，参数走 `ebx/ecx/edx`；涉及调度（exit/sleep/yield）的调用不返回。
- PIC 掩码（v0.10）：允许 IRQ0（定时器）、IRQ1（键盘）、IRQ4（串口 COM1 接收），
  串口中断经 `serial_irq` 读走全部可用字符并转发给注册的接收钩子。

## 4. 进程调度（v0.5 核心）

- **PCB**：pid、状态(READY/RUNNING/BLOCKED/ZOMBIE/FREE)、内核栈、用户栈、唤醒 tick、退出码。
- **就绪队列**：定长环形缓冲（`sched_policy.c`，纯逻辑、可宿主单测），
  约定"运行中的进程不在队列"，入队队尾、出队队头 → 自然构成轮转(RR)。
- **上下文切换**：中断现场即进程现场。
  - 被抢占：`schedule()` 把 `kernel_esp` 指向当前中断帧（gs 槽），压入就绪队尾。
  - 切出：`sched_switch_esp(kernel_esp)` 直接 `mov esp, target` + `jmp resume_point`，
    `resume_point` 与中断返回同路径（pop gs..ds / popa / iret）。
  - 选下一进程后先更新 `tss.esp0 = 目标进程内核栈顶`，保证 ring3 下次中断能回到正确内核栈。
  - v0.11：选下一进程后同步 `switch_page_dir(n->page_dir)` 写 CR3（自动刷 TLB），
    切到目标进程的独立地址空间；idle/内核进程用内核页目录（`page_dir=0`）。
- **调度点**：
  - `sched_tick`：定时器抢占 + 唤醒到期阻塞进程 + 回收僵尸。
  - `sched_yield`：主动让出；`sched_sleep`：阻塞 n tick；`sched_exit/kill`：置 ZOMBIE。
  - 关键约束：`schedule()` 在"当前为 idle 且就绪队列为空"时会**正常返回**，
    调用方必须据此返回而非停机（见 bugs.md BUG-002）。
- **idle 进程**：PID 0，ring0 运行，`sti; hlt` 等待中断，负责状态刷新与键盘回显，
  仅在无任何就绪进程时被切入。

## 5. IPC 与同步（v0.6 核心）

### 信号量（sem.c，纯逻辑可单测）
- 对象：`count` + FIFO 等待队列（`waiters[]`）。
- **与调度器解耦**的接口设计，把"簿记"和"调度动作"分开：
  - `sem_wait_try(s, pid)`：`count>0` 递减并返回 0（占用成功，继续运行）；
    `count==0` 把 pid 入队并返回 1（**应阻塞**）；队列满返回 -1。
  - `sem_signal_wake(s)`：队列非空则取出队首 pid 返回（**应唤醒**，count 不变，
    资源直接交给被唤醒者）；否则 `count++` 返回 `SEM_NO_PID`。
- **经典语义**：signal 唤醒等待者即"把资源交给它"，被唤醒进程不再执行 wait，
  由 `iret` 直接恢复用户态并返回 0。

### 与调度器对接（syscall 层组合）
- `sys_sem_wait`：`sem_wait_try` 返回 1 → `sched_block(r, BLOCK_SEM)`（不返回，现场存中断帧）。
- `sys_sem_signal`：`sem_signal_wake` 返回 pid → `sched_wake(pid)` 置就绪并入队。
- PCB 新增 `block_reason`（`BLOCK_NONE/BLOCK_SLEEP/BLOCK_SEM`）：
  `sched_tick` 只唤醒 `BLOCK_SLEEP`（定时），sem 等待者只能被显式 `sched_wake` 唤醒，
  从机制上杜绝"定时器误唤信号量等待者"（见 bugs.md BUG-004）。
- **唤醒返回值**：`sched_wake` 把被唤醒进程保存帧的 `eax` 置 0，
  阻塞系统调用恢复后正确返回 0（见 bugs.md BUG-005）。

### 共享内存（sys_shmem）
- 所有进程共享同一页表，映射同一物理帧到固定虚拟地址 `0x80020000 + slot*4K` 即互通。
- slot 按需惰性分配物理帧（`frame_alloc` + `map_page(..., flags=0x7)`）。

### 用户演示（procSemA/procSemB）
- **rendezvous 会合**：`signal(a2b); wait(b2a)` 与对称侧形成双向等待，
  无论调度顺序如何都能完成会合且不死锁。
- **互斥共享计数**：临界区 `wait(mutex); (*cnt)++; sleep; signal(mutex)`，
  持锁 sleep 强制对端阻塞，最终 `cnt` 恰为 10，验证互斥正确无竞争丢失。

### 消息队列（msg.c，纯逻辑可单测）
- 对象：环形缓冲（容量 1..8）+ 双 FIFO 等待队列（生产者/消费者）。
- **与调度器解耦**的接口设计与信号量同风格：
  - `msg_send_try(q, val, pid)`：缓冲有空位 → 入队返回 0；满 → 把 `{pid, val}`
    **暂存**进生产者队列返回 1（应阻塞）；队列满返回 -1。
  - `msg_recv_try(q, out, pid)`：非空 → 取出返回 0；空 → pid 入消费者队列返回 1；队列满 -1。
  - `msg_send_wake(q)`：入队成功后调用，若有等待消费者，把消息**直接交付**给它
    （返回消费者 pid 与消息值；消费者 recv 直接返回该消息，缓冲不滞留）。
  - `msg_recv_wake(q)`：取出成功后调用，若有暂存生产者且有空位，把其消息**搬入缓冲**
    并返回生产者 pid（send 由内核代发，视为成功）。
- **恰好一次送达**：两种唤醒都保证一条消息只给一个等待者；
  生产者阻塞时消息已在队列中暂存，不会因唤醒时机丢失。
- 系统调用 `sys_msg_create/send/recv`：`send` 阻塞用 `sched_block(r, BLOCK_MSG, id)`，
  `recv` 唤醒用 `sched_wake_with(pid, 消息值)`（PCB 的 `block_arg` 记录队列 id 便于定位）。
- **用户演示（procMsgP/procMsgC）**：生产者快产慢消、消费者先建先等，
  同时演示 send-block（缓冲满）与 recv-block（缓冲空）；20 条消息按序恰好一次送达。

## 6. 文件系统（v0.8 核心）

### 块设备抽象（blockdev.c/h）
- 以 4KB 块为单位的 `read/write/ptr` 接口，屏蔽后端差异；
  当前后端为**内存盘（ramdisk）**——由 `frame_alloc_run` 申请一段连续物理帧，
  物理地址即线性地址（落在内核低 16MB 恒等映射区），`blockdev_ptr` 直接返回块内指针免拷贝。

### 磁盘布局（mini-fs，类 Unix）
```
块 0        超级块   (magic "MINI" / 总块数 / inode 数)
块 1        inode 位图   (64 inode -> 8B)
块 2        数据块位图   (<=252 块 -> 32B)
块 3        inode 表     (64 * 64B = 4KB)
块 4..      数据块
```
- 只支持根目录（inode 0）、单级目录，文件名 <= 23 字符。
- inode：`size / type / links / blocks[12]` 直接块映射 → 单文件最大 48KB。
- 目录：128 条/块，缺块自动扩容（`dir_add` 在无空条目时分配新数据块清零）。

### 文件操作语义
- `fs_create`：查重名 → 分配 inode → 写入根目录（失败回滚位图）。
- `fs_write`：按需分配覆盖范围内所有数据块（新块清零）；仅前插补，不写洞；
  跨块边界自动切块；更新 size。
- `fs_read`：按 size 截断，跨块切块读取，返回实际读到的字节数。
- `fs_delete`：释放所有数据块 → 清目录条目 → 释放 inode（位图回收）。
- `fs_list`：遍历根目录数据块，收集 `{name, inode}`。

### 系统调用（13~19）与打开文件表
- 内核维护 `fs_files[8]`（槽 0 保留）：`{used, inode, pos, mode}`。
- `open(slot, name, mode)` 绑定槽位，write/read 从 `pos` 推进（顺序 IO），
  `close` 释放槽位；用户以固定槽位引用已打开文件，免去句柄分配/查找。
- 用户演示：
  - **procFSA**：create hello.txt → 写 8000 字节（跨块）→ close → 读回逐字节校验 → verify OK
  - **procFSB**：create alpha.txt/beta.txt → 写 alpha.txt → 内核 ls 打印根目录 → 完成

## 7. 可执行程序加载与 Shell（v0.9 核心）

### 应用构建与内嵌（initramfs）
- 应用（`src/apps/`：hello/echo/crash/shell）**独立编译**为 ELF，
  用 `-Ttext` 链接到固定地址（普通应用 `0x80040000`，shell `0x80030000`），`-e app_main` 指定入口。
- 通过 `objcopy -I binary` 把**完整 ELF 文件**内嵌进内核（保留文件头，供内核解析），
  启动时 `initramfs_setup` 把 motd + 各应用 ELF 作为文件写入 ramdisk。
- 注意：必须内嵌 ELF 本身而非 `objcopy -O binary` 的裸镜像（后者剥掉文件头，无法解析段）。

### ELF 加载器（elf.c，纯逻辑可单测）
- 解析 ELF32 文件头/程序头，仅取 `PT_LOAD` 段：按链接地址放置、覆盖写段数据、
  `memsz > filesz` 部分清零（bss）。
- `mapfn` 钩子：逐页申请物理帧并映射到目标虚拟地址，加载器与具体页表实现解耦。
- `elf_load_range`：预先扫描 PT_LOAD 段，返回页对齐的 `[base, end)` 覆盖区间，
  供 `load_elf_file` 决定映射范围——**必须包含 ELF 头所在页**
  （`-Ttext` 会把 ELF 头放到目标地址的前一页，见 bugs.md BUG-007）。
- `load_base=0` 时按链接地址原样放置（绝对寻址），应用天然自洽，无需重定位。

### 加载、spawn 与资源回收
- `usermode_spawn_elf(name, vbase, resident)`：读文件 → `elf_load` 映射并拷贝 →
  `sched_spawn_at(entry, name, frames, fcount, vbase)` 建进程。
  - `resident=1`（shell）：代码帧不记入 PCB，进程退出也不回收（常驻）。
  - `resident=0`（普通应用）：代码帧移交 PCB 的 `own_frames[]`，进程退出自动 `frame_free`。
- 系统调用：
  - `sys_spawn_file(name)`(21)：加载 ELF 到 app 槽，返回 pid（app 槽忙/加载失败返回 -1）。
  - `sys_wait(pid)`(22)：等待子进程退出，返回退出码；已退出直接返回，否则 `BLOCK_WAIT` 阻塞，
    exit 时唤醒等待者并携带退出码。
  - `sys_readline(buf,max)`(20)：阻塞式读一行；无行时 `BLOCK_KEYBOARD` 阻塞并记录用户缓冲，
    行就绪后由 `sched_wake_keyboard` 拷入缓冲再唤醒。

### 键盘行缓冲（kb.c）
- 行缓冲与字符环形缓冲解耦：idle 继续负责回显，用户进程按行消费。
- 退格处理、行完成回调 `kb_set_line_hook`（挂 `sched_wake_keyboard`）、
  `kb_line_take(out,max)` 取走一行（互斥），状态在测试间可复位。

### 常驻 shell（shell.c）
- 命令：`help / ls / cat <file> / run <prog> / exit`。
- `run`：`sys_spawn_file` 启动应用 → `sys_wait` 等退出 → 打印退出码；
  演示动态加载 ELF + 父子进程同步；echo 演示用户态阻塞 readline；crash 演示内存保护隔离。

## 8. 串口终端（v0.10 核心）

### 输入源统一：kb_feed_char
- 键盘 IRQ1 把扫描码查表成 ASCII 后，调用 `kb_feed_char(c)` 组装行缓冲；
  串口 IRQ4 收到字符后，同样调用 `kb_feed_char(c)`——**键盘与串口共用同一行缓冲**，
  阻塞式 `sys_readline` 无感知地同时支持两种输入源。
- `\r` 与 `\n` 均视为定行（串口终端回车常只发 `\r`）；退格删行尾；行完成触发回调唤醒等待者。

### 串口接收通道（serial.c）
- `serial_init` 只开"接收数据可用"中断（IER=0x01），波特率 38400 8N1，FIFO 使能。
- `serial_irq`（IRQ4）：接收中断触发后，循环 `serial_rx_ready()` 读走 FIFO 里所有字符，
  逐字符调用接收钩子 `rx_hook`（内核注册为 `kb_feed_char`）。
- PIC 掩码从 `0xFC` 改为 `0xEC` 放开 IRQ4。

### 外部 agent 交互
- `qemu-system-i386 -serial stdio`：QEMU 把虚拟串口接到标准输入输出，
  外部进程（终端/脚本/另一个 AI agent）向 QEMU stdin 写入即输入，读 stdout 即输出。
- 内核串口输出（serial_printf/puts）与输入回显都在同一通道，天然构成双向终端。
- `tests/test_serial.sh` 用一对 FIFO 模拟 agent 通道：agent 发 `help/ls/cat motd/run hello/run echo/run crash`，
  轮询串口日志断言各命令回显与输出——验证"终端通道"端到端可用（与键盘 sendkey 路径互补）。

## 9. 每进程地址空间与物理内存隔离（v0.11 核心）

### 动机
v0.5~v0.10 所有进程共享同一页表：任何进程都可访问其他进程的用户栈/代码页，
"隔离"只依赖代码自觉。v0.11 为每个进程建立**独立页目录**，进程间用户半区映射互不可见，
真正实现物理内存隔离。

### 地址空间模型
- 每个用户进程在 spawn 时 `addr_space_create()`：克隆内核页目录**低 512 个 PDE**
  （低 1GB：恒等映射低 16MB + 懒分配区），清空用户半区（PDE 512..1023）。
- 内核半区页表与内核页目录**共享**（克隆的是 PDE 值，指向同一批页表帧）：
  任何地址空间下内核代码/数据/堆/ramdisk/共享页均可直接访问（都在低 16MB 恒等映射区）。
- 用户半区页表**进程独占**：`map_page_in(pd, virt, phys, flags)` 写入目标页目录，
  页表帧/页目录帧都分配在低 16MB 恒等映射区，故任何上下文都能安全写入任意进程的页目录。
- `switch_page_dir(pd)`：写 CR3（写 CR3 自动刷新 TLB）；`pd=0` 视为内核页目录。
- `addr_space_destroy(pd)`：释放该页目录独占的页表帧 + 页目录帧。

### 各环节接入
- **上下文切换**：`schedule()/sched_start()` 选下一进程后 `switch_page_dir(n->page_dir)`。
- **ELF 加载**：`usermode_spawn_elf` 先 `addr_space_create` → 把 CR3 切到目标页目录 →
  `elf_load` 直接写入目标地址空间（`app_mapfn` 用 `map_page_in(load_pd, ...)`）→ 切回。
  这避免旧方案"临时映射进父进程页目录"覆盖父进程自身映射（父子链接到同一虚拟地址时必崩）。
  **关键**：CR3 切换期间必须 `cli`，且切回后**不再 sti**（引导期过早开中断会触发 BUG-009）。
- **系统调用**：内核代码/栈/堆都在恒等映射区，任何地址空间（任何 CR3）下都能正常运行，
  syscall 处理器无需切页表；进程运行时 CR3 即其私有页目录，`map_page` 按"当前 CR3"定位页目录。
- **共享内存**：物理共享帧只分配一次并清零，但**每次 `sys_shmem` 都重新映射进当前进程页目录**
  （v0.11 起各进程页表不再共享，首次分配时的映射只对首个调用者有效）。
- **进程名**：父进程传入的 name 可能位于其用户地址空间，子进程退出/回收时 CR3 已切到子进程
  页目录，直接读父进程地址会缺页 → PCB 增加 `name_buf[16]`，spawn 时拷入内核内存。
- **资源回收**：进程退出时回收 `map_frames[]`（sys_map_page 申请的私有页）+ `addr_space_destroy`。

### 隔离验证（isol 应用）
- 第一个实例经共享内存槽 1 设旗标，生成孪生实例（同一 ELF，独立地址空间）。
- 两个实例都 `sys_map_page(0x80050000)`：同一虚拟地址 → **不同物理页**。
- 各自写入 `1000+pid` → 睡眠交错 → 读回仍是自己写的值 → `ISOLATED OK`。
- QEMU 回归用正则统计日志中该虚拟地址对应的不同物理页数量（≥2 即通过）。

## 10. 测试策略（工程化）

### 宿主单元测试（tests/run_host_tests.sh）
- 把无内核依赖的纯逻辑编译成普通 Linux 程序断言：
  `heap.c`、`kb.c`、`sched_policy.c`、`sem.c`、`msg.c`、`blockdev.c + fs.c`、`elf.c`。
- 优点：秒级反馈、可用 ASan/valgrind、无需 QEMU。
- 用 `-fno-pie -no-pie` 保证 32 位地址假设在宿主环境成立。

### QEMU 自动回归（tests/qemu_regression.sh）
- 无图形界面运行内核 N 秒 → 抓串口日志 → 正则校验关键里程碑。
- 覆盖：进程创建/spawn/切入、A/B 抢占打印、sleep/wake、crash 隔离、僵尸回收、idle 心跳、
  v0.6 信号量创建/等待阻塞/唤醒/共享内存/rendezvous/互斥自增、
  v0.7 消息队列创建/消费者阻塞/生产者阻塞/生产者唤醒/收发完成、
  v0.8 内存盘初始化/文件创建/写模式打开/跨块写入/读回校验通过/多文件创建/ls 列出/演示完成、
  v0.9 initramfs 写入/内核加载 shell/readline 阻塞/交互式注入 shell 命令
  （经 HMP monitor `sendkey` 端到端跑 `help/ls/cat motd/run hello/run echo/run crash`）。

## 11. fork/exec 进程模型与 argv（v0.12 核心）

### sys_fork：地址空间深拷贝
- 分配空闲 PCB 槽（`alloc_pid` 扫描 FREE，退出后 pid 可重用）→ 新内核栈 → `addr_space_create()`。
- 遍历**父进程页目录用户半区**（PDE 512..1023）每个映射页：
  - **共享内存区**（`[SHMEM_VBASE, +SLOTS*4K)`，mem.h 常量）：保持同一物理帧（fork 后父子共享）；
  - **其余全部深拷贝**：`frame_alloc` 新帧 + `memcpy` 内容 + `map_page_in` 映射进子页目录，
    新帧记入 `PCB.fork_frames[]`（退出时统一回收）。
  - 页表帧/页目录帧在低 16MB 恒等映射区，遍历与写入可在任意 CR3 下进行。
- **子进程现场** = 父进程当前中断帧副本 + `eax=0`（fork 返回 0）；用户 esp 指向父进程用户栈，
  已被深拷贝，同一虚拟地址内容一致 → 子进程从 fork 调用点继续，局部变量/调用链原样。
- PCB 的 `own_frames/map_frames` 在 fork 子进程保持空：深拷贝帧统一在 `fork_frames`。

### sys_exec：镜像替换
- `sys_exec(name, argc, argv)`：**先在当前（旧）地址空间把 name 与 argv 内容拷入内核缓冲**
  （`namebuf`/`names[8][64]`）——随后加载/替换会切 CR3，旧用户内存不再可读（BUG-011）。
- 创建新地址空间 → `cli` + 切 CR3 → `load_elf_file` 加载到新地址空间 → 切回。
- `sched_exec`：分配新用户栈帧映射到新 pd → `argv_layout` 按 cdecl 布置 → 释放旧地址空间
  （`release_priv_frames` + `addr_space_destroy`）→ 立即切 CR3 到新 pd → 复用**当前 pid 与内核栈**
  重建 PCB（name/entry/own_frames）→ **就地改写当前中断帧**为新程序入口现场 → `schedule` 切走，
  下次切回时 iret 从新入口执行。exec 成功不返回。

### argv 的 cdecl 布置（argv_layout）
- 在新用户栈顶从高到低布置：**字符串区 → argv 指针数组(n+1) → argv 指针槽 → argc 槽 → fake_ret(esp)**。
- 进入 `app_main(int argc, char **argv)` 时：`[esp]=返回地址, [esp+4]=argc, [esp+8]=argv 指针数组地址`。
- 关键：`argv` 参数是"指针数组的地址"（需独立指针槽），不是数组首元素（BUG-012 教训）。

### 关键点
- `sched_exec` 里 `set_name` 必须在释放旧地址空间前（name 可能指向旧用户栈，BUG-010）。
- exec 复用内核栈与 pid：释放的是用户资源（ELF/私有页/fork 帧/旧栈/旧页目录）。
- 资源回收统一：`release_priv_frames(p)` 释放 own/map/fork/stack 帧，terminate/reap/exec 复用。

### 演示
- `forkdemo`：fork 后父子把同一虚拟地址映射到不同物理页 → 双 `ISOLATED OK`（深拷贝铁证）。
- `args`：打印 argc/argv（argv[0]=程序名）。
- shell `exec <prog> [args...]`：fork 子进程 → 子进程 exec → 父进程 wait，经典全链路。

## 12. 用户栈守卫页与栈溢出检测（v0.13 核心）

### 动机
v0.12 fork/exec 已能创建任意进程，但用户栈只有 4KB 且没有任何下溢保护——
程序 `int a[4096]` 或深递归即可静默踩穿栈底，破坏相邻数据/映射。v0.13 给
**每个用户栈的栈底加一个未映射的守卫页**，下溢即触发页错误，内核识别为栈溢出并隔离终止。

### 布局（mem.h）
```
用户栈区 [USER_STACK_AREA_BASE, +MAX_PROCS*8K)
  每进程 8KB 槽（按 pid 错开）：
    槽内低半页 [0, 4K)   = 守卫页（不映射，陷阱）
    槽内高半页 [4K, 8K)  = 栈页（映射）
  栈从槽顶向下增长：下溢越过栈页底部 → 落入守卫页 → 页错误
```
- `USER_STACK_SLOT=0x2000`（8KB）、`USER_STACK_GUARD=0x1000`（4KB）
- `spawn/spawn_at/exec` 只映射 `stk = vbase + GUARD` 这一页，守卫页天然不映射
- `user_esp_top = vbase + SLOT`（栈顶）；共享内存区 `SHMEM_VBASE` 后移至
  `0x80044000`，位于栈区（16 槽 × 8K = 0x80010000..0x80030000）与 shell/app 槽之后

### 判定（guard.c，纯逻辑可宿主单测）
- `stack_guard_hit(fault)`：地址在栈区内且 `(fault & (SLOT-1)) < GUARD`（槽内低半页）→ 命中。
- 只依赖 mem.h 布局常量，不碰任何内核/硬件 → 编译进宿主测试 `test_guard`（15 条边界断言）。

### 接入 pf_handler
- 用户态页错误先判 `stack_guard_hit(fault)`：
  - 命中 → 打印 `[user] STACK OVERFLOW pid=.. @.. -> killed`，`sched_kill` 终止该进程；
  - 未命中 → 原有"PAGE FAULT -> killed"路径（越权/非法访问隔离）。
- 对栈的**合法使用**无影响：正常压栈/调用不越界，不触发守卫页。

### 演示
- `stackovf`：读 esp → 向下 8KB 对齐得本进程栈槽基址（即守卫页）→ 写入该未映射页 →
  页错误 → 内核 `STACK OVERFLOW` 打印 → 进程被终止（`exited code=`），系统其余进程不受影响。

## 13. 关键设计取舍

| 取舍 | 理由 |
|------|------|
| 多进程共享同一份代码页 | 无需按进程加载 ELF，聚焦"调度/切换/隔离"本身（v0.5 范围） |
| 就绪队列用定长环形数组 | 简单、可穷举边界、方便宿主单测 |
| 中断帧复用为进程现场 | 避免额外保存大量寄存器，切换路径与中断返回完全一致 |
| 纯策略与硬件驱动分离 | 让最难测的逻辑（堆/队列/映射）可脱离硬件测试 |
| 信号量"簿记"与"调度"解耦 | sem.c 纯逻辑可单测；阻塞/唤醒动作由 syscall 层组合，职责单一 |
| 固定 id 信号量槽 + 固定共享页地址 | 演示阶段免去复杂的对象注册/查找，聚焦同步语义本身 |
| 内存盘代替磁盘 | 无需写 IDE/AHCI 驱动即可先跑通文件系统语义，blockdev 接口隔离后端以便日后换真磁盘 |
| 直接块映射（无间接块/多级索引） | 布局简单、可穷举边界、方便宿主单测；单文件 48KB 对演示足够 |
| 打开文件表用固定槽位 | 用户以槽位引用免去句柄分配/查找，聚焦顺序 IO 语义本身 |
| 应用按链接地址原样放置（load_base=0） | 静态链接 + 固定地址即可运行，无需重定位；应用"天然自洽" |
| 应用单独编译为 ELF、内嵌原始文件 | 保留文件头供内核解析；独立编译互不干扰，便于增加新应用 |
| resident 常驻 shell + 一次性应用槽 | shell 常驻免于反复加载；普通应用退出即回收代码帧，简单防泄漏 |
| 键盘行缓冲与环形缓冲解耦 | idle 继续负责回显，用户进程按行消费；阻塞/唤醒语义清晰 |
| 串口接收经钩子复用键盘行缓冲 | 输入源统一、零重复代码；`sys_readline` 无需感知键盘还是串口 |
| `-serial stdio` + FIFO 模拟 agent 通道 | 外部 agent/脚本可完全无头驱动内核，回归可复用同一通道 |
| 每进程独立页目录 + 内核 PDE 克隆共享 | 用户半区真正隔离，内核半区零复制（共享同一批页表帧）；只多一份页目录+独占页表帧开销 |
| 页表/页目录帧落在低 16MB 恒等映射区 | 任何地址空间（任何 CR3）都能直接读写任意进程的页表，`map_page_in` 可在任意上下文安全调用 |
| ELF 加载期间切 CR3 直写目标地址空间 | 避免临时映射进父进程页目录覆盖其自身映射；加载全程关中断，防止抢占后 CR3 错乱 |
| 共享页每次 sys_shmem 重映射进当前页目录 | 各进程页表不再共享后，重映射保证所有进程都能访问同一共享物理帧 |
| 栈槽固定 8KB（守卫页 4K + 栈页 4K）、按 pid 错开 | 守卫页=未映射陷阱页实现栈下溢检测，零运行时开销；栈页仍可深拷贝/fork，槽位索引 = pid 使栈区定位 O(1) |
| `stack_guard_hit` 抽成纯逻辑文件 guard.c | 只依赖布局常量、可宿主单测边界；pf_handler 只做"命中即 kill"的组合动作 |
| fork 用"立即深拷贝"而非写时复制(COW) | 教学内核聚焦 fork 语义本身；COW 需额外缺页处理 + 帧引用计数，留待后续 |
| 共享内存区在 fork 时保持共享 | 符合共享内存语义：fork 只隔离"私有"内存，共享页父子继续互通 |
| pid 槽位扫描重用（alloc_pid） | 并发演示（fork/isol 等）会耗尽单调递增的 next_pid；退出后复用槽位让 MAX_PROCS 够用 |
| exec 就地改写中断帧 + 复用 pid/内核栈 | 无需新建进程；`schedule` 保存的就是改写后的现场，切回即执行新程序 |
| argv 按 cdecl 布置在新用户栈 | 应用入口即 `app_main(argc, argv)`，零启动代码，GCC 直接按标准调用约定取参 |
