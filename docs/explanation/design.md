# 架构设计与开发思路（Design）

## 1. 总览

Mini-OS 是 x86 32 位保护模式内核，通过 multiboot 协议由 QEMU 直接加载
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
   ├─ ata_init()      v0.16 探测 IDE 真盘（无盘则回落纯内存盘）
   ├─ storage_init()  v0.16 ramdisk + 真盘加载/格式化 + initramfs（见第 14 节）
   └─ sched_init/spawn/start → 多进程调度接管（不返回）
       └─ usermode_spawn_elf("shell", ...)  v0.9 加载常驻 shell
```

## 2. 内存布局

| 区域 | 说明 |
|------|------|
| 0x100000 ~ 内核末尾 | 内核代码/数据（保护页） |
| 物理帧分配器 | 以 4KB 页为单位，位图/链表式分配 |
| 0x80000000 | 用户空间基址（USER_SPACE_BASE）；v0.11 起每进程独立页目录 |
| 0x80010000 ~ 0x80090000 | 用户栈区：16 槽 × 32KB = [槽底硬底守卫页 4K（永不映射）\| 28KB 可生长栈区]（v0.26 栈按需生长；栈从槽顶向下增长，初始仅顶页映射） |
| 0x80090000 | 常驻 shell 的链接/加载地址（v0.9 定址，v0.26 随栈区扩迁至此；resident 帧不随退出回收） |
| 0x800A0000 ~ 0x801A0000 | app 区 1MB（v0.26#3 扩区去上限；`run <prog>` 加载的 ELF 按自身 PT_LOAD 逐页映射，退出回收代码帧；cc500 亦链接到 0x800A0000 基址） |
| 0x801A0000 + slot*4K | 共享内存页（SHMEM_VBASE=app 区之后；所有进程映射同一物理帧到同一虚拟地址） |
| 0x801A4000 ~ 0x801F4000 | 用户堆区 320KB = 80 页（v0.26#2 `sys_brk`；扩展按页补映射、收缩保留映射复用） |
| 0x81000000 | 用户空间上界（USER_SPACE_END，v0.26#3 扩至 16MB） |
| 每进程 4KB 内核栈 | 中断切换用，TSS.esp0 指向其栈顶 |

- 懒分配：用户/测试页首次访问触发页错误，`pf_handler` 检查懒分配区并补映射，随后重试；
  v0.26 起用户栈按需生长（STACK_GROWTH）与堆页补映射也走这条路径。
- 内核地址区写保护：ring3 越权写内核页 → 页错误 → 进程被终止（v0.4 起有 crash 演示）。
- v0.11 起：**每个用户进程持有独立页目录**（见第 9 节），不再共享页表；
  各进程用户半区（≥2GB）映射互不可见，内核半区（低 16MB 恒等映射 + 懒分配区）克隆共享。
- 共享内存机制（v0.6 设计，v0.11 适配）：物理共享帧只分配一次，但**每次调用 `sys_shmem`**
  都把该帧重新映射进当前进程页目录，保证各进程都能访问同一物理帧。
- 用户地址空间总上界 `USER_SPACE_END=0x81000000`（16MB）：栈区/共享内存/app 区/堆区
  全部落在其内（v0.26#3 扩容，各段基址见 [mem.h](../../v2-c-kernel/src/mm/mem.h) 布局注释）。

## 3. 中断与系统调用

- IDT：32 个 CPU 异常 + 16 个硬件 IRQ + `int 0x80` 系统调用门（DPL=3，允许用户态触发）。
- `isr_common_stub` 统一入口：压入寄存器现场 `registers_t`（gs..eax、中断号、eip/cs/eflags/user_esp/ss）。
- 系统调用分发 `syscall_dispatch`：`eax=号`，参数走 `ebx/ecx/edx`；涉及调度（exit/sleep/yield）的调用不返回。
- PIC 掩码（v0.10）：允许 IRQ0（定时器）、IRQ1（键盘）、IRQ4（串口 COM1 接收），
  串口中断经 `serial_irq` 读走全部可用字符并转发给注册的接收钩子。
- **单核假设**（并发模型）：本内核面向**单 CPU**（无 SMP），中断是唯一的抢占源，
  内核临界段靠"关中断"（`cli`/`sti`）串行化。因此信号量/消息队列的
  "try + block" 两步虽非原子，在单核 + 中断串行模型下安全（`sched_block` 保存现场后
  才开中断，无并发交错）；`sem/msg` 模块也据此不额外加锁。若未来引入多核或嵌套中断，
  需在 `sem_wait_try`+`sched_block`、`msg_send_try`+`msg_send_wake` 之间加 `cli/sti` 保护。

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

### 文件系统增强（v0.14 核心）

#### 目录层级与路径解析
- 目录操作全部泛化为"指定目录 inode"：`fs_lookup_in(bd, dir, name)` /
  `fs_list_dir(bd, dir, out, max)` / `dir_add / dir_remove(bd, dir, ...)`，不再写死根目录。
- `fs_mkdir` / `fs_rmdir`：建/删目录；`fs_rmdir` 仅允许**空目录**（扫描条目，非空/非目录拒绝）。
- 路径解析器 `fs_walk(path, &dirout, leaf, ...)`：
  - 按 `/` 拆分组件逐级下钻；支持 `.`（当前）、`..`（父目录，用**显式目录栈**回退；
    根目录的 `..` 仍是根）、重复/结尾斜杠。
  - 输出 `*dirout`=叶子所在目录、`*leaf`=叶子名（路径以 `/` 结尾时为空串）。
  - 返回叶子 inode；叶子缺失 / 中间组件不存在或非目录 / 层级过深均返回 -1，
    **且失败路径也保证写出 leaf/dirout**（否则调用方读未初始化栈值，见 bugs.md BUG-015）。
- `fs_create / fs_lookup / fs_delete / fs_list` 全部路径化（平铺名即根目录文件，向后兼容）。

#### 间接块（inode 扩展）
- `fs_inode_t` 增加 `indirect` 字段 + pad 对齐到 64B（仍 64 个/块）。
- `file_block(bd, in, b, create)`：块号 `< 12` 走直接块；`>= 12` 走间接块
  （`indirect` 指向一块存 1024 个块号，惰性分配）。单文件上限 12+1024 块 ≈ 4.1MB。
- `fs_write` 先确保覆盖范围块已分配（`file_block(create=1)` 对新块清零），
  `fs_read` 用 `file_block(create=0)` 按块寻址，跨块自动切块。
- `free_inode_blocks`：删除/rmdir 时释放直接块 + 间接块指向的全部数据块 + 间接块本身。

#### 偏移定位 / 追加写
- `sys_fs_open(slot, name, mode)`：mode 0=只读 1=只写 2=**追加**（`pos=文件尾`）。
- 新增 `sys_fs_seek(slot, off)`（SYS_FS_SEEK=26）定位读写位置；
  `sys_fs_mkdir`(27) / `sys_fs_rmdir`(28)。
- `sys_fs_ls(path)`：路径化并按 `type` 打印，目录带 `/` 标记；shell 新增
  `mkdir / rmdir / rm` 命令，`ls [path]` / `cat <path>` 路径化。
- 演示 `fsdemo`：mkdir /etc、/etc/sub → 子目录建文件 → 追加两段配置 → seek 读回校验
  "8080" → 100000B 大文件（跨入间接块）4 处偏移抽查 → rmdir 拒绝非空 → 逐级清理。
  输出约定：**每行单次 `sys_print`**（原子行），避免被抢占时其它进程输出拆断日志行。

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

## 10. 测试策略（工程化，v0.18 起五层）

### 宿主单元测试（tests/run_host_tests.sh）
- 把无内核依赖的纯逻辑编译成普通 Linux 程序断言：
  `heap.c`、`kb.c`、`sched_policy.c`、`sem.c`、`msg.c`、`blockdev.c + fs.c`、`elf.c`、
  `guard.c`、`brk.c`、`userptr.c`、`netutil.c`、`ip.c`、`udp.c`、`icmp.c`、`dhcp.c`。
- 优点：秒级反馈、可用 ASan/valgrind、无需 QEMU。
- 用 `-fno-pie -no-pie` 保证 32 位地址假设在宿主环境成立。

### QEMU 自动回归（tests/qemu_regression.sh，键盘 HMP sendkey 路径）
- 无图形界面运行内核 N 秒 → 抓串口日志 → 正则校验关键里程碑。
- 覆盖：进程创建/spawn/切入、A/B 抢占打印、sleep/wake、crash 隔离、僵尸回收、idle 心跳、
  v0.6 信号量创建/等待阻塞/唤醒/共享内存/rendezvous/互斥自增、
  v0.7 消息队列创建/消费者阻塞/生产者阻塞/生产者唤醒/收发完成、
  v0.8 内存盘初始化/文件创建/写模式打开/跨块写入/读回校验通过/多文件创建/ls 列出/演示完成、
  v0.9 initramfs 写入/内核加载 shell/readline 阻塞/交互式注入 shell 命令
  （经 HMP monitor `sendkey` 端到端跑 `help/ls/cat motd/run hello/run echo/run crash`）、
  v0.12~v0.15 fork/exec/argv/栈守卫/waitdemo/exec 失败反馈、v0.16 的 `selftest` 单行自检，
  以及 v0.17~v0.27 的 abuse 边界被拒 / stack 生长 / brk·heapdemo / bigdemo 去上限 /
  ccboot 自举闭环（P1 被加载运行 + 退出码 + `byte-identical PASS`）。

### 串口终端回归（tests/test_serial.sh，串口 agent 通道）
- 以 FIFO 模拟"外部 agent 通道"，经 `qemu -serial stdio` 驱动 shell 逐命令交互，
  校验命令回显与各输出里程碑（help/ls/cat motd/run hello/echo/crash/isol/forkdemo/exec/
  stackovf/fsdemo/waitdemo/selftest）。与键盘路径互补，验证"终端通道"。
- v0.27b 起新增「写-编-跑」用例：`writefile /hello.c <源码>` → `ccrun /hello.c /hello.elf`
  → 编译产物被加载运行（`[elf] '/hello.elf' loaded`）→ 退出码 PASS。

### ATA 持久化回归（tests/test_persist.sh，v0.16 新增）
- **两次 QEMU 运行共享同一 `-hda` 磁盘镜像**：
  第 1 次格式化空白盘 → `mkdir /persist` → `save` 写回 → 退出；
  第 2 次重启挂载同一镜像 → 校验 `/persist` 仍在、且持久盘上的应用可经 `selftest` 正常运行。
- 这就是"OS 报告 → agent 修改 → make test → QEMU 重启 → 结构化回归"闭环里的"重启验证"环节。
- v0.27 起再补「工具链 × 持久化」组合格：`writefile` + `ccrun` 出的编译产物 `/persist/p.elf`
  经 `save` 落盘，重启后 `run /persist/p.elf` 仍能加载运行（跨子系统回归盲区补格）。

### 网络回归（tests/test_net.sh，v0.18 新增第五层）
- QEMU `-device e1000` + SLIRP + `filter-dump` 抓包 pcap；校验串口日志里程碑
  （e1000 探测/链路、DHCP 四步、ARP/UDP/ICMP 自检、`netping`/`recvfrom` 交互），
  并用 python 解析 pcap 作为**外部预言机**独立核验线上包（ARP 双向 / UDP / ICMP 计数）。
- 与宿主纯逻辑单测（netutil/ip/udp/icmp/dhcp）配合：协议编解码逻辑在宿主测、线缆语义在 QEMU 测。

### 单行结构化自检（shell `selftest`，v0.16 新增）
- shell 内置 `selftest` 命令：逐跑 hello/isol/forkdemo/fsdemo/waitdemo 五个代表应用
  （覆盖 spawn/隔离/fork/FS/wait），每项打印退出码；v0.21 追加第 6 项内核自审计
  （`SYS_KERN_AUDIT`：帧配平/信号量守恒/PCB 状态机）。最后汇总**一行**：
  `[selftest] PASS (6 checks)` 或 `[selftest] FAIL: N/6 checks`。
- 外部 agent 只 grep 这一行即可全量确认，避免逐条关键字匹配几十个里程碑。

### 回归盲区的教训（fsdemo BUG-016）
- 关键字断言只能验证"某行出现了"，验证不了"不变量"（如退出码）。
  fsdemo 曾"通过"测试却以退出码 -1 + 误导性 STACK OVERFLOW 退出，因为回归只 grep `[fsdemo] done`。
- v0.16 起系统性地：给各应用补**退出码断言**（`'<app>' exited code=0`），并用 `selftest`
  按退出码汇总，把"可见输出匹配"提升为"行为不变量校验"。

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

## 13. wait 语义与孤儿清理（v0.15 核心）

### sys_wait：经典 wait/waitpid
- 签名：`sys_wait(pid, *status)` —— 返回**被回收的子进程 pid**，退出码写 `*status` 出参。
- `pid=-1`（`wait()`）：等待任意子进程；`pid` 具体（`waitpid(pid)`）：等待该子进程。
- **只回收自己的子进程**：校验 `ch->parent_pid == 当前进程`；别人的僵尸/已回收/非法 pid → -1。
- 快速路径：已有 ZOMBIE 子进程 → 立即 `sched_reap` + 返回 pid。
- 阻塞路径：目标子进程还活着（或无退出但存在存活子进程）→ `sched_block(BLOCK_WAIT, pid或-1)`，
  并把 `block_arg2` 记为 `*status` 出参指针。
- 无子进程可等 → 返回 -1（waitdemo 第三次 `wait(-1)` 即验证此分支）。

### 唤醒与退出码交付
- `terminate_current` 唤醒等待者：匹配 `block_arg == 子 pid` **或** `block_arg == -1`（任意等待者）。
- 返回值为子 pid（`sched_wake_with(q->pid, p->pid)`），退出码经 `*status` 出参交付：
  运行在子进程上下文（CR3=子页目录），故**临时切到父进程页目录**写入 `*status`，再切回
  （与 `sched_wake_keyboard` 写 readline 缓冲同理）。
- 交付后把子进程置 `parent_pid=0`，僵尸交心跳回收（BUG-014 的延迟回收机制复用）。

### 孤儿清理
- 父进程退出时，把其所有子进程 `parent_pid` 置 0（孤儿化），由心跳回收。
- 必要性：若父先退出且其 pid 槽被 `alloc_pid` 复用于新进程，孤儿（parent 指向新进程）
  永远等不到"父 FREE"而被回收，造成泄漏。

### 演示（waitdemo）
- fork 3 个子进程（退出码 7/9/11）→ 父进程循环 `wait(-1, &code)` 依次回收：
  打印每次返回的 pid 与 code；校验 3 个 pid 互异、退出码集合 {7,9,11}（`verify OK`）；
  全部回收后再 `wait(-1)` 返回 -1。原子行输出便于回归断言。

## 14. 用户态 CRT 收口 + ATA 持久化 + 单行自检（v0.16 核心）

### 用户态 CRT 收口（app_main 返回即退出）
- 新增 `src/apps/crt.c`：ELF 入口由 `app_main` 提升为 `_start`——
  `_start(argc, argv)` 取参调用 `app_main`，返回后统一 `sys_exit(0)`。
  根治"app_main 忘了 sys_exit 就从栈槽顶未映射处 ret"这类崩溃（BUG-016），
  各应用不再需要手写尾部 `sys_exit(0)`。
- 内核配套（sched.c `entry_block`）：spawn 路径把入口 cdecl 块
  `[fake_ret][argc][argv]` 写在栈页顶下方 12B（esp = 槽顶-12），保证
  `_start` 读 argc/argv 时 `[esp+8]` 仍在已映射栈页内；无参数启动用 argc=0/argv=0。
- 教训：改入口符号后，凡是"新入口会读参数"的代码，都必须保证对应地址已映射
  （CRT 首次引入时 shell 因读 `[esp+8]` 越页直接页错误，经 entry_block 修复）。

### ATA PIO 驱动（src/ata.c）
- 主通道 master，LBA28，轮询模式（不依赖中断）：IDENTIFY(0xEC) 探测扇区数、
  读(0x20)/写(0x30) 按扇区（512B）。带 BSY/ERR/超时保护；无盘立即返回 0。
- QEMU：`-hda disk.img` 即挂 PIIX IDE 盘；无 `-hda` 时探测失败、回落纯内存盘。

### 存储子系统（src/storage.c）——持久化分水岭
- **有盘**：整盘读入 ramdisk → 超级块 magic 有效则**直接挂载**（跳过格式化/initramfs，
  磁盘即真源，用户数据跨重启存活）；空白盘则格式化 + initramfs，并落盘一次。
- **无盘**：纯内存盘（v0.8 原行为，重启丢失）。
- `storage_sync()`：把 ramdisk 全量写回磁盘（SYS_FS_SYNC=29 / shell `save`）。
- 扇区级搬运用 512B 静态缓冲，避免内核栈大数组。

### 单行结构化自检（shell `selftest`）
- 见 §10 测试策略；价值在于把"几十个关键字断言"收敛成"一行 PASS/FAIL"，
  是"OS 主动报告自身健康"的机器可读通道。

## 15. syscall 边界校验（copyin/copyout，v0.17 核心）

### 动机
内核按低地址恒等映射（内核内存全部 < 0x80000000），用户内存位于高地址半区。
v0.17 之前，syscall 处理器直接解引用用户传入的指针：一个恶意/出错的应用可以传
内核地址（如 0x100000、0xB8000）让内核替它读写内核内存——借 `sys_fs_write` 把
内核数据拷进文件、或借 `sys_readline` 往内核地址写，绕过页保护。

### 校验层（src/userptr.c/h，纯逻辑可宿主单测）
- `user_ptr_valid(p, len)`：先做**区间/回绕**判定（`p >= USER_SPACE_BASE(0x80000000)` 且
  `p <= END(0x81000000)` 且 `len <= END - p`，先判 `p > END` 防止减法回绕），再对 `[p, p+len)`
  **逐页 `is_mapped()`** 确认已映射。这是用户指针校验的**单一收敛点**：下面 6 处直接解引用
  syscall 与 `copyin/copyout` 全走它——命中"区间内空洞页"时在**触碰内存前**即返 0（防内核态缺页
  [FATAL] 整机停机）。`len` 可为 0（空指针须单独判定）。
- `copyin / copyout`：内部即 `user_ptr_valid` 预检，通过后直接 memcpy（当前 CR3 即用户页目录，
  用户半区已映射，内核可直接寻址）；失败返回 -1，不触碰任何内存。
- `copyin_str`：长度未知（到 NUL 为止），不套定长前预检，逐字节拷时**跨页处 `is_mapped()`** 检查
  （含非页对齐起始页），与 `user_ptr_valid` 共用同一谓词，仍受映射保护。
- 上界 END=0x81000000（v0.26#3 用户空间扩 16MB）比实际映射区宽裕，属安全侧收紧；
  重点是**拒绝内核低地址 + 拒绝区间内空洞页**。

### 接入点（usermode.c）
- 字符串入参：`print / spawn_file / fs 的 create/open/ls/delete/mkdir/rmdir`、
  `exec` 的 name 与每个 argv[i]——一律 `copyin_str` 进内核缓冲后再用。
- 缓冲/出参：`fs_write / fs_read / readline` 的缓冲、`wait` 的 status——先
  `user_ptr_valid` 再让内核直接读/写（大缓冲不搬进内核栈，4KB 栈放不下）。
- `exec` 的 argv 指针数组先 `user_ptr_valid(argv, argc*4)`，再逐项 `copyin_str`。

### 演示（abuse）
- 用 0x100000（内核数据区）/0xB8000（显存）/0xFFFFFFF0（回绕）调用各类 syscall，
  全部返回 -1 被拒；合法路径（建/写/删文件）正常，证明"只挡越权、不误伤"。

## 16. 关键设计取舍

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
| `-serial stdio` + FIFO 模拟 agent 通道 | 外部 agent/脚本可在无界面模式（无图形窗口、仅串口输出）下驱动内核，回归可复用同一通道 |
| 每进程独立页目录 + 内核 PDE 克隆共享 | 用户半区真正隔离，内核半区零复制（共享同一批页表帧）；只多一份页目录+独占页表帧开销 |
| 页表/页目录帧落在低 16MB 恒等映射区 | 任何地址空间（任何 CR3）都能直接读写任意进程的页表，`map_page_in` 可在任意上下文安全调用 |
| ELF 加载期间切 CR3 直写目标地址空间 | 避免临时映射进父进程页目录覆盖其自身映射；加载全程关中断，防止抢占后 CR3 错乱 |
| 共享页每次 sys_shmem 重映射进当前页目录 | 各进程页表不再共享后，重映射保证所有进程都能访问同一共享物理帧 |
| 栈槽固定 8KB（守卫页 4K + 栈页 4K）、按 pid 错开 | 守卫页=未映射陷阱页实现栈下溢检测，零运行时开销；栈页仍可深拷贝/fork，槽位索引 = pid 使栈区定位 O(1) |
| `stack_guard_hit` 抽成纯逻辑文件 guard.c | 只依赖布局常量、可宿主单测边界；pf_handler 只做"命中即 kill"的组合动作 |
| 目录操作泛化为"指定目录 inode" + 绝对路径解析 | 目录层级是"目录即数据块集合"的自然扩展；路径解析保持单一职责（解析器只管下钻与 .. 回退） |
| inode 直接块 + 单级间接块 | 布局简单、可穷举边界、可宿主单测；单文件 4.1MB 对演示充足；多级间接块/索引节点留待后续 |
| 僵尸延迟回收（父进程存活时保留） | 修复 sys_wait 在"spawn 后、wait 前被心跳抢先回收"导致返回 -1 的竞态；boot/孤儿进程仍由心跳回收防泄漏 |
| sys_wait 返回 pid + status 出参（pid=-1 等任意） | 对齐经典 wait()/waitpid 语义；waitdemo 能演示"回收任意子进程 + 拿回 pid/退出码" |
| 父退出时子进程孤儿化 | 避免父 pid 槽被复用后孤儿永远无法回收的泄漏；与延迟回收机制互补 |
| 唤醒时临时切父地址空间写 status 出参 | 退出码交付发生在子进程上下文（CR3=子页目录），需切 CR3 才能写父进程用户内存（复用 readline 唤醒同款做法） |
| fsdemo 每行单次 sys_print（原子行） | 多进程并发输出共享串口，拆行会破坏回归关键字匹配；拼一行再打印天然原子 |
| fork 用"立即深拷贝"而非写时复制(COW) | 教学内核聚焦 fork 语义本身；COW 需额外缺页处理 + 帧引用计数，留待后续 |
| 共享内存区在 fork 时保持共享 | 符合共享内存语义：fork 只隔离"私有"内存，共享页父子继续互通 |
| pid 槽位扫描重用（alloc_pid） | 并发演示（fork/isol 等）会耗尽单调递增的 next_pid；退出后复用槽位让 MAX_PROCS 够用 |
| exec 就地改写中断帧 + 复用 pid/内核栈 | 无需新建进程；`schedule` 保存的就是改写后的现场，切回即执行新程序 |
| argv 按 cdecl 布置在新用户栈 | 应用入口即 `app_main(argc, argv)`，零启动代码，GCC 直接按标准调用约定取参 |
| CRT 收口：ELF 入口为 `_start`（app_main 返回即 sys_exit） | 根除"忘写 sys_exit 从栈顶 ret 崩溃"整类问题，统一各应用退出语义（v0.16） |
| spawn 路径入口 cdecl 块写在栈页顶下方 12B | 保证 `_start` 读 argc/argv 时 `[esp+8]` 已映射；无参启动用 argc=0/argv=0（v0.16） |
| ATA PIO（轮询）代替 DMA/中断 | 教学内核聚焦块设备语义本身；QEMU 下 PIO 足够快，无中断/PIC 依赖（v0.16） |
| 整盘读入 ramdisk + 显式 `save` 写回 | FS 逻辑零改动复用（仍按 blockdev 内存寻址）；持久化=按需落盘，符合"先跑通再优化"（v0.16） |
| 持久化判定用超级块 magic | 空白盘/有效盘可区分；首启格式化后自动落盘一次（v0.16） |
| 扇区级搬运用 512B 静态缓冲 | 避免内核栈放 4KB 大数组（KSTACK=4KB 会溢出）（v0.16） |
| shell `selftest` 单行结构化汇总 | 让 agent grep 一行即完成全量验证；把"可见输出匹配"提升为"退出码不变量校验"（v0.16） |
| syscall 一律先校验用户指针再使用 | 防恶意/出错应用借 syscall 读写内核内存（copyin/copyout）；大缓冲只校验不搬运，避免 4KB 内核栈溢出（v0.17） |
| 校验只判"指针落在用户半区"（含上界防回绕） | 内核全部位于低地址、用户位于高半区，该条件即安全边界；END 取宽裕值仅作回绕保护（v0.17） |
| copyin_str 逐字节拷贝并在越界处 -1 | 防"无 NUL 字符串读到缺页"；路径/名字先拷入内核缓冲再解析（v0.17） |

## 17. 网络子系统（v0.18~v0.28 核心）

### PCI 配置空间 + e1000 驱动（v0.18）
- **PCI type-1 配置空间**（`src/drv/pci.c/h`）：`pci_config_read/write`（端口 0xCF8/0xCFC）、
  `pci_find(vendor, device)` 扫描总线 0 找网卡、`pci_bar_alloc_mem`——探测 BAR 大小
  （全 1 写回再读）、在 PCI MMIO 洞（0xFEB00000 起）分配地址并写回、使能 MEM|BUSMASTER。
  QEMU `-kernel` 不经 SeaBIOS，BAR 须驱动自分配。
- **e1000**（`src/drv/e1000.c/h`，Intel 82540EM）：MMIO BAR0 恒等映射进内核页目录；
  软复位（CTRL.RST）→ 强制链路（CTRL.SLU）→ 轮询 STATUS.LU；MAC 从 RAL0/RAH0 读取；
  legacy 16B 描述符环（RX16/TX8），**轮询收发**（无 DMA 中断）：`e1000_tx` 填描述符
  （EOP|IFCS|RS）→ 写 TDT 触发 → 轮询 status.DD 确认；`e1000_rx` 轮询 DD → 拷缓冲 → 归还 RDT。
- 三条硬件坑（见 bugs.md）：描述符环须 `volatile`（否则 -O2 把 status 读提升到循环外，
  BUG-018）；TCTL/RCTL 的 EN 位是 bit1（0x2）不是 bit0（BUG-019）；QEMU 写 RCTL 会启动
  1000ms flush 定时器、期间包进队列不进 RX 环（BUG-020，初始化末尾等窗口过期）。
- **纯逻辑以太网/ARP 帧**（`src/net/netutil.c/h`，44 断言）：`net_build_arp_request`（广播）、
  `net_eth_type`、`net_parse_arp_reply`；启动自检 `e1000_selftest` 发 ARP（who has 10.0.2.2）
  收 SLIRP 网关回复，端到端验证 TX+RX。

### 极简 IP/UDP（v0.19，纯逻辑可宿主单测）
- `src/net/ip.c/h`：IPv4 头构建/解析 + RFC 1071 16 位校验和（`ip_checksum`）。
- `src/net/udp.c/h`：完整帧构建（Ethernet+IPv4+UDP，校验和含 12B 伪头）/解析（round-trip + 拒绝路径）。
- 校验和基准由独立 python 参考实现算得，宿主单测 test_ip(24)/test_udp(24)。

### 用户态 UDP socket（v0.20）
- `sys_net_socket/sendto/recvfrom/close`（30-33）；内核 `netsock` socket 表 + 网卡轮询分发
  （recv 先排空 NIC 再取队首，非阻塞与轮询驱动对齐）；`netio.h` 共享 iov 结构
  （3 参 syscall 承载多参，ABI 与内核一致）。
- `sockdemo` 用户态端到端回环（socket→sendto PING→轮询 recvfrom PONG）。
- **坑**：e1000 MMIO 位于高地址（PDE≥512）、进程页目录只克隆低 1GB PDE → syscall 路径缺页；
  netsock 收发前临时切内核页目录（BUG 见 v0.20 修正）。

### 自审计与边界契约（v0.21）、netping（v0.22）、ICMP（v0.23）、UDP 校验和（v0.24）、DHCP（v0.25）
- `SYS_KERN_AUDIT`(34)：`sem_invariant_ok`（count+waiters 守恒）、`mem_audit`（used_frames 与
  帧位图配平）、`sched_audit`（PCB 状态机合法性）；selftest 追加第 6 项。
- shell `netping [ip] [port]`：开 UDP socket 发 PING、轮询收 PONG，单行原子打印
  `[netping] <ip>:<port> PONG +<N>B rtt=<T> ticks`。
- `src/net/icmp.c/h` 纯逻辑：Ethernet+IPv4+ICMP Echo（校验和只覆盖 ICMP 报文，RFC 792 无伪头）；
  `e1000_icmp_selftest` 发 Echo 到 SLIRP 网关收回显（`[icmp] echo reply … rtt=N ticks`）。
- UDP 接收端校验和验证（RFC 768）：伪头+UDP 头+载荷重算须折叠为 0 才接受；校验和字段 0=
  发送端未计算 → 接受；发送端算得 0 以 0xFFFF 发送（两者不混淆）。坏包在 `udp_parse` 即 -1 静默丢弃。
- `src/net/dhcp.c/h` 纯逻辑（RFC 2131/2132）：`dhcp_build_discover`（0.0.0.0:68 → 广播，option 55
  参数请求列表 + flags=0x8000）、`dhcp_build_request`（server id 54 + 请求 IP 50）、`dhcp_parse_reply`
  （校验 xid/magic cookie/消息类型 53，提取 IP/网关/租期）；`e1000_dhcp_run` 开机四步状态机
  （DISCOVER→OFFER→REQUEST→ACK，忙等超时 ~2s、NAK/超时重试），失败回退静态
  `NET_STATIC_IP`/`NET_STATIC_GW`；`e1000_my_ip()`/`e1000_gw_ip()` 供 ARP/UDP/ICMP 三自检取动态 IP。

### DHCP 租期续约（v0.28，RFC 2131 §4.4.5）
- **T1/T2 定时**：ACK 后记录租期并 tick 化 T1=0.5×lease（单播 RENEW）、T2=0.875×lease
  （广播 REBIND）；`e1000_dhcp_tick()` 由 timer 心跳每 tick 非阻塞驱动（`timer_cb` 里、
  `sched_tick` 前，保证不被上下文切换跳过）。
- **状态机**：`RENEW_NONE → RENEW_SENT → REBIND_SENT →（REACQ_OFFER/REACQ_ACK）`；
  到 T1 发单播 RENEW（ciaddr + 54 + 50），到 T2 未 ACK 升广播 REBIND（仅 50，任意服务器
  可续）；ACK 重置定时器继续下一租期；NAK / REBIND 超时 → 重新 DISCOVER→OFFER→REQUEST→ACK
  重新获取 → 彻底失败回静态兜底。每 tick 至多"发一帧 + 收一帧"，绝不在 ISR 忙等。
- **端口 68 专用接收端点**（`netsock_dhcp_open/recv`）：用户 socket 的 recvfrom 会
  "排空"网卡（netsock_drain 取走 NIC 环所有帧），无匹配本地端口的 DHCP 应答被抢先丢弃；
  注册端口 68 的 DHCP socket 后，分发路径把应答入其队列，续约 tick 经它读取。
- **两条坑**：① e1000 MMIO 位于高地址（PDE≥512），timer ISR 可能在用户进程页目录
  （只克隆低 1GB PDE）下运行 → 访问设备寄存器缺页；tick 内临时切内核页目录（与 netsock
  收发同款），用完切回。② `print_ip` 逐字节须 `& 0xFF`（否则 `10.2560.655362...`）。
- **可测性**：Makefile `DHCP_RENEW_SECS`（缺省用服务器租期，SLIRP 为 24h）编译期覆盖
  租期为秒级，test_net 短租期内核在秒级窗口断言 RENEW→ACK 续约闭环（pcap UDP 10→12）。

### 网络抽象层 v1.1（netif + 虚拟 TCP，路标四步）
- **核心解耦**：协议层不再直调网卡。`src/net/netif.{c,h}` 提供 ops 表（init/ready/tx/rx/mac）
  + 注册表，包单位为 **IP 数据报**（lwIP netif 模式）；以太网头 / SLIP 帧等链路层封装**下沉到
  网卡适配层**（`e1000_netif.c` 追加以太头、`uart_netif.c` 做 SLIP 成帧）。`netsock`/`dhcp` 等
  协议层只走 `netif_tx/rx`，`grep e1000_ src/net/` 为空由 CI 守卫强制。管理面：`netif_select`/
  `UART_NETIF_DEFAULT` 静态绑网卡（D6）。
- **虚拟 TCP = 薄包装 + 宿主转发器**（真 TCP 状态机只在宿主，见 roadmap「语义边界」诚实声明）：
  - **会话协议**（`src/net/tcp_proto.h`）：8B 会话头（session_id/msg_type/version/flags，大端），
    msg_type 0x01 DATA / 0x02-05 事件(host→guest) / 0x06-07 控制(guest→host)；单一事实来源
    （guest C + 宿主 Python + fuzz 三方共用）。三份语义规定定稿 docs/（session-proto / thin-api /
    mtu-fail）。
  - **guest 薄包装**（`src/app/tcp.c`，用户态库）：`tcp_open/send/recv/close` + `tcp_wait_open`，
    fd=连接对象（事件环 + 数据环 + 状态机 FREE→OPENING→OPEN→CLOSED/ERROR），`tcp_recv` 三态
    >0/0/-1 互斥（0=对端关闭，-1=失败/超时）。薄包装发送硬墙 = netsock sendto/recvfrom 钳制
    1400（收发数据报 ≤1400），转发器下行分块 ≤1392，`NET_RXMAX=2048` + 环 4096 重组 >1KB 响应
    不丢尾（BUG-044）。
  - **宿主转发器**（`tests/tcp_proxy.py`）：会话表 + UDP↔TCP 映射 + 事件回传 + 超时/半开/背压，
    支持 UDP(e1000)+SLIP(COM2) 双通道；主循环**非阻塞 multiclient select**（BUG-045 修正阻塞饿死）。
  - **demo/回归**：`httpdemo` 开机 HTTP demo（成功 200 + 拒绝 -1）；test-tcp 双通道 + >1KB/尾字节
    完整性断言；test-slip 串口网卡；fuzz case7 会话头解析。

## 18. 容量三连（v0.26 核心）

### #1 用户栈按需生长（`src/kernel/guard.c` + `src/mm/mem.c` pf_handler）
- 每进程栈槽由 8KB 固定扩为 **32KB 槽 = 槽底硬底守卫页 4K（永不映射）+ 28KB 可生长栈区**，
  栈从槽顶向下增长、初始仅映射顶页；命中守卫页时内核补映射新栈页、守卫页随栈底下移，
  直到槽底硬底（此时深越界才判溢出）。
- `stack_guard_hit` 由 v0.13 二态扩为三态 `STACK_OK / STACK_GROWTH / STACK_BOOM`（纯逻辑宿主单测）：
  GROWTH=命中当前守卫页且槽内仍有空间 → 补映射重试；BOOM=深越界或已到硬底 → 隔离终止。
- PCB `stack_frame` → `stack_frames[]` + `stack_fcount` + `stack_bottom`；`stack_init`/`stack_free`
  统一管理，覆盖 spawn / exec / fork / reap 全路径。
- `deep` 演示：递归分配 1KB 局部数组 ×12 层，在 4KB 初始栈上触发 3 次按需生长后存活。

### #2 用户堆（brk/sbrk，`src/mm/brk.c/h` + `src/kernel/usermode.c`）
- `SYS_BRK`(35)：`sys_brk(addr)`（0=查询当前 brk）/ `sys_sbrk(incr)`（相对增长返回旧 brk）；
  堆区 `[USER_HEAP_BASE=0x801A4000, USER_HEAP_MAX=0x801F4000)` 320KB=80 页，每进程独立隔离。
- `brk.c` 纯逻辑（`brk_pages_up` / `brk_in_range`）可宿主单测；扩展按页补映射物理帧并记账进
  PCB `heap_frames[]`+`heap_fcount`，收缩只更新 `heap_brk` 保留映射复用。
- **容量守卫（S8）**：按"目标 top 自 heap_base 起的页数 ≤ USER_HEAP_PAGES"判定——不用
  `brk_pages_up(old,a)`（旧 brk→新 brk 跨度），否则收缩后旧映射保留、再涨过同一段会重复计数、
  在真实预算内误拒；映射页恒为 `[base, top)` 前缀，单调增长下两式等价。
- **BUG-025**：扩展映射从非页对齐 `old` 起逐 0x1000 上跳，brk 落在页中部时顶部半页未映射
  → 任意非页对齐 malloc 越界缺页（heapdemo 页对齐侥幸绕过；cc500 任意尺寸命中）。
  修复：映射 `[old,a)` 相交的所有页（old 下取整、a 上取整），与记账一致。

### #3 ELF 加载去上限（`src/kernel/usermode.c` load_frames + `sched.c` own_frames）
- `load_frames` 固定 8 项静态数组 → 按需 `kmalloc` 动态列表；`own_frames` 同步动态化
  （`own_frames_take` 拷入 PCB、`release_priv_frames` 归还），解除 32KB/8 帧约束；
  `APP_MAXFRAMES`/65536B 旧上限移除（上限改为 app 区同量级 1MB）。
- 地址空间重布局：用户空间扩至 16MB（`USER_SPACE_END=0x81000000`）、app 区 1MB
  （0x800A0000-0x801A0000）、共享内存迁至 0x801A0000、堆区 0x801A4000。
- `bigdemo`：70KB 初值数据（.data 段）使 ELF 78KB > 旧上限，加载需 21 帧（旧 8 帧时代无法加载）。

## 19. 工具链与自举（v0.27 / v0.27b 核心）

### cc500 编译器移植（`tools/cc500/cc500.c`，~877 行）
- 移植 E. Grimley-Evans 自托管 C 子集编译器（~750 行上游，GPL-2.0 参考/自写）：stdin 读 C →
  stdout 出 x86-32 ELF，无 libc 依赖；头注释逐条列移植改动、保留原作者声明。
- 链接基址 `code_offset=0x800A0000`（APP_LINK），ELF 头 e_entry/p_vaddr/p_paddr 同步改址。
- **唯一机器码 stub = 通用系统调用** `syscall3(n,a,b,c)`（eax=n ebx=a ecx=b edx=c，int $0x80）；
  `exit/malloc/getchar/putchar/sys_print` 全用 CC500 C 子集实现（malloc 基于 SYS_BRK bump），
  编译器内部不含平台相关代码；该 stub 同时发射进每个被编译程序，编译器与产物共享同一 ABI 实现。
- **专用 CRT**（`src/app/cc500_crt.c`）：`_start` 以 `cc500_main()` 返回值 `sys_exit`
  （普通 crt.o 固定退出 0，无法把编译成败传给 shell）；不 include user_lib.h
  （其 static inline syscall3 会与外部 syscall3 重名冲突）。
- **语言子集约束**：无 `break/continue/for/switch/&&/||/!/</>/%/类型转换`，循环用 done 标志退出、
  `>=` 用操作数交换为 `<=`、换行用 `\x0a`——自举源必须能被自己编译。
- **I/O 走 mini-fs**：整读输入文件（v0.27b 起 `argv[1]`，缺省 `/cc500.c`）进堆做 getchar 源；
  `putchar` 为空操作，编译完 `flush_output` 把 code 缓冲写回输出路径（`argv[2]`，缺省 `/out.elf`）
  ——绕开无 stdin/stdout 重定向限制。
- **输入大小守卫（S6）**：`in_data=malloc(32768)`；缓冲满后再探 1 字节，仍有数据则显式
  `input too big (>32KB)` 报错，杜绝静默截半编译。
- initramfs 嵌入：`cc500`（编译器 ELF）+ `cc500.c`（自举源，objcopy 原始字节）两个文件。
- **BUG-026**：畸形输入（形参列表 EOF 未闭合）死循环——符号表无界增长；加 EOF 守卫。

### 自举不动点（shell `ccboot`）
- 步骤：run cc500（gcc 版）编译自身 → `/out.elf`=P1；**快照 P1 到 /p1.elf**（P2 会覆盖 /out.elf，
  时序处理干净）；run `/out.elf`（=P1）再编译自身 → P2；`file_equal` 对 **/p1.elf 与 /out.elf
  做真·逐字节比对**（长度 + 内容循环），输出 `[ccboot] byte-identical PASS/FAIL`。
- 比较对象是"cc500 版 vs cc500 版"（不是 gcc 输出 vs cc 输出）——正确的不动点定义；
  PASS 仅在两文件等长且逐字节相同时打印（S4 从 FNV 哈希升级为逐字节）。

### 写-编-跑闭环（v0.27b shell 命令）
- `writefile <path> <content>`：命令行剩余部分（保留空格）写入文件，agent 可在 guest 内经
  shell 写源码（ARG_MAX 32→128）。
- `ccrun <src> <out>`：fork+exec cc500 编译 `<src>` 为 `<out>` → `run <out>` → 校验退出码，
  端到端「写-编-跑」一键。完整剧本：`writefile /hello.c <源码>` → `ccrun /hello.c /hello.elf`
  → 编译产物被加载运行（`[elf] '/hello.elf' loaded`）→ 退出码 0；cc500 对任意合法源程序
  编译出可运行 ELF，不再局限于编译自身。

## 20. 加固与工程化收口（v0.29~v0.33 核心）

> v0.29 起进入"阶段二·加固"：不给新功能，只让回归更可信、更可定位。四条主线——
> 宿主 fuzz 把"畸形输入"变成大规模被测（v0.29）；工具链真 bug 复现实锤 + 版本单一来源（v0.30）；
> 共享内核资源归属收口（v0.31）；自举编译器缺陷修复 + 回归可观测性 + CI 全链（v0.32/v0.33）。

### 宿主侧 fuzz + 内核堆审计（v0.29）

- **确定性 fuzz**（`tests/fuzz_parse.c`，挂 `run_host_tests.sh` 第 16 项）：xorshift32 确定性
  PRNG（固定种子可复现）对纯逻辑解析模块注入随机路径/随机字节，ASan+UBSan 下断言"畸形输入被
  拒绝而不崩溃"。覆盖 `fs_walk`（随机路径与写/建/删/列混合）、`elf_load_range`（畸形头/段表
  越界读）、`net_eth_type`/`net_parse_arp_reply`/`ip_parse`/`udp_parse`/`icmp_parse`（帧内
  载荷指针越界）、`dhcp_parse_reply`（畸形选项长度）；FS 内存盘每 4096 轮重置防 inode/块耗尽。
  缺省 60000 轮（36 万次解析调用），`FUZZ_ITERS` 可调。设计动机：把解析模块从"被测过几次"
  变成"被测过几十万次"，畸形输入成本降到接近零。
- **内核堆审计**（`src/mm/heap.c/h` `heap_audit()`，挂入 `kern_audit`）：遍历 `block_t` 链表
  校验 magic/free 一致性、size 上界，块数超上界即判 next 成环停止（防死循环）；新增
  `used_bytes`/`free_bytes` 记账计数器与遍历统计**对账**——泄漏（块游离于计数外）、双重释放
  （计数提前减）、写越界破坏块头的场景都会使两者漂移而暴露；报告碎片（空闲块数/字节）。
  宿主 `test_heap.c` + QEMU selftest `[audit] heap ok` 双重锁定（守恒断言：
  `4 blocks, free 3 blocks/102320B used 16B pages=25`，used+free+4×16B 头 = 25×4096 精确守恒）。
- **fuzz/补格抓到两 bug**：BUG-029 `icmp_parse` 短帧越界读（`len < 14` 越过帧尾 + 无符号下溢
  成巨大值 → 堆越界读；开头 `if (len < 14) return -1`）；BUG-030 fork 子进程在继承的已生长栈上
  递归被误判缺页（`sched_fork` 继承 `stack_bottom` 但 `stack_guard_hit` 按子 pid 反推槽位 →
  槽位改由**实际栈位置** `stack_bottom & ~(USER_STACK_SLOT-1)` 推导，普通进程=自身槽、fork
  子进程=继承的父槽）。
- **回归盲区补格**：`deepfork`（已生长栈×fork）与 `deepexec`（已生长栈×exec）演示挂双层断言；
  `test_guard.c` 补 4 条 fork 继承栈守卫断言。方法论：每发现一类"逻辑正确但边界未测"的盲区，
  立即补演示应用 + 宿主断言，把盲区变成常驻回归。

### 工具链严重 BUG 修复与版本单一来源（v0.30）

- **BUG-031 全局文件槽泄漏污染工具链**：`fs_files[8]` 全局表无进程归属、退出不清理——cc500
  一次 parse error（裸 `exit(1)` 跳过 `flush_output`）即永久占用 slot2，此后所有编译
  `setup_output` 失败直到重启。修复：文件槽记**打开者 pid**，`terminate_current` 调
  `fs_files_close_pid(pid)` 按归属归还；**不**关闭其他并发进程的槽（首版"关全部槽"被 repro
  抓到误伤 procSemB 与 P1 并存场景——"精准归还"而非"全清"是资源回收的正确粒度）。
- **BUG-032 cc500 自编译产物静默丢 argv**（`be_start`）：入口桩裸 `call` 不编组 argc/argv，
  自编译产物 exec 带 argv 时静默走默认路径写 /out.elf 且退出 0。修复：`call` 前把内核栈
  `[esp+4]=argc、[esp+8]=argv` 压给首函数，与 cc500 "首参 8(%esp)/末参 4(%esp)" 约定精确对齐；
  `e_entry` 不变、`call` rel32 回填 85→95。
- **代码审查三连**：BUG-033 `map_page_in` 页表帧 OOM 返回 0 未检查 → `(uint32_t*)0` 清零低
  4KB 且页目录项指向物理 0（改返回 int，`pf_handler` 检查失败转 `STACK_BOOM` 并释放已分配帧）；
  BUG-034 kb 行缓冲 `line_ready` 期间仍追加输入（仅 `!line_ready` 才入缓冲）；BUG-035
  `fork_frames[24]` 硬编码限制大进程 fork（改 kmalloc 动态数组，同 `own_frames`）。
- **`repro_bugs.sh` 双断言**：BUG-A（good.c 编译 OK → 坏源 FAIL → 同源 good2.c 再编译必须成功，
  证槽不污染）与 BUG-B（ccboot 产 P1 → exec 带 argv 再编译 → /out2.elf 必须被创建）。修复后
  实测 `[ls] out2.elf size=19217`（argv 生效）。复现实锤文化：外部报告 bug 先固化为可复现脚本，
  再谈修复。
- **版本串单一来源**（L-4/L-5）：新增 `src/version.h` 的 `MINI_OS_VERSION`，内核启动横幅 /
  shell banner / initramfs motd / 回归断言统一取宏——消灭"改版本只改了一处"的漂移。
- **BUG-036 编译卫生教训**：`-w` 压不住 permerror——GCC 14 中 `-Wint-conversion` 是硬错误，
  必须显式 `-Wno-int-conversion`（GCC 13 下 `-w` 单独够用是曾误删此项的根源）。

### 内核资源归属收口：per-process fd + socket 归属（v0.31）

- **per-process fd 表入 PCB**（`sched.h/usermode.c/sched.c`）：`pcb_t` 增
  `fd_table[FS_FDS_PER_PROC]`，fd 号从"全局约定号"改为"本进程私有号"；`sys_open/read/write/
  close` 一律在当前进程自己的 fd 表上做，`sys_fork` 深拷贝子进程 fd 表、`exec/exit` 清本进程
  fd 表——根治 v0.30 方案仍留的跨进程槽号互污染（v0.30 记 pid 是过渡，收进 PCB 才是终态）。
  `procFSB` 改用与 `procFSA` 相同的 `fd=1` 打开自己的文件作负对照。
- **socket 归属/回收**（F-0a/b/c，见 bugs.md BUG-037/038）：`net_sock_t` 增 `pid` +
  `reserved`；`terminate_current` 调 `netsock_close_pid(pid)` 归还其所有 socket（此前开 socket
  不关即退出使槽位永久失踪直到表满网络降级）；`case 33` 改 `netsock_close_if_owner`——仅可关
  本进程打开、非保留的槽，**端口 68 的 DHCP 保留槽拒绝关闭**，续约链不再被打断；
  `netsock_audit()` 并入 `kern_audit`（占用计数 + 保留槽恒计数），创建失败加"表满"专项日志。
- **攻击回归**（`tests/test_socket.sh`，挂 `make test`）：F-0a 退出回收（leak2 后 netping 仍
  PONG）+ F-0b 保留槽防 close + 观测断言，修复前后红→绿区分成立。设计主线：共享资源
  "谁打开谁所有、退出必归还"，先于并发模型本身（见 22 章门纪律）。

### 自举编译器三缺陷修复（v0.32）

- **hostcc 基座**（`tools/cc500/host_crt.c`）：把 cc500 编成 Linux 宿主程序——缺陷与内核无关，
  宿主秒级红绿 + gdb 可调（先于 QEMU 全量回归定位）。
- **F-3 未闭合字符串自噬**（BUG-039）：`get_token` 无 EOF 守卫 + `primary_expr` 无 NUL 守卫 →
  越界读写直到堆里偶遇 `"`（前：guest 内核击杀 exit=-1 / hostcc SIGSEGV rc=139）。修复：两处
  守卫，命中即 `cc500: bad string` 干净报错。
- **F-2 只声明未定义函数静默编废产物**（BUG-040）：纯原型声明不触发 `sym_define_global`
  回填，符号恒留 `'U'=code_offset`，调用即 `PAGE FAULT`。修复：`be_finish` 收尾遍历符号表
  （锚点=名字 NUL 位），检出残留 'U' 且 `value != code_offset` → `cc500: undefined symbol`。
- **F-1 关系运算残缺 + error() 零诊断**（BUG-041）：只识别 `<=`。修复：`error()` 打印
  `cc500: error at <token>`；补齐 `<`/`>`/`>=`（setle=0x9e/setl=0x9c/setge=0x9d/setg=0x9f，
  操作数序 objdump 实测锁定）；字符串解码补 `\n`/`\t`。
- **症状对立断言**（`tests/test_cc500.sh`）："新症状必须出现 + 旧症状必须缺席"杜绝假绿——
  宿主 T 系列 + `<` 编码 `0f 9c` 锁定；guest 自举不动点 P1==P2 + `<` 运行语义保持。
- 全回归绿：宿主 16/16 + QEMU + 串口 + 持久化 + 网络 + socket + cc500 + repro_bugs；尺寸锚点
  `entry=800a0054` 未动（无硬编码字节断言，产物随代码体积自然漂移）。

### 回归可观测性收口 + harness 语义 + CI 全链（v0.33）

- **F-4 selftest 汇总行撕裂**（BUG-042）：PASS/FAIL 汇总行多次 `sys_print` 拼，片段间可被内核
  异步打印（孤儿 reap / DHCP 续约）插入撕裂 → 整行锚定回归假阴性。修复：走 `nl_*` 缓冲原子行
  （一次 flush，与 netping/ccboot/writefile 同机制）——行原子性是回归可观测性的地基（22 章
  uart 归属）。
- **F-5 pid 表耗尽静默**（BUG-043）：`alloc_pid` 耗尽静默 `return -1`，三处调用方对 `pid<0`
  无声返回（fork 炸弹先到的是无声槽耗尽而非有日志的深拷贝 OOM）。修复：`alloc_pid` 每耗尽
  周期报一次 `[sched] pid table full`（防风暴刷屏）+ `sched_audit` 补 `slots=%u/MAX_PROCS`。
- **harness 退出码语义统一**：7 个测试脚本统一"缺依赖 → `[ERR]` + exit 2"（此前缺 socat 等会
  退化成全断言超时 `[FAIL]/exit 1`，**环境病伪装代码病**）。约定：`0` 全绿 / `1` 断言失败 /
  `2` 环境或依赖缺失（CI 显式将 `2` 标环境错误）。
- **CI 全链 + 分步矩阵**：单 job `make test` 全 7 层门禁 + 失败上传 build-logs 工件 +
  `workflow_dispatch`；新增 `layers.yml` 并行矩阵（host/qemu/serial/persist/net/socket/cc500
  每层独立 job 定位）——矩阵定位与单链门禁并存，既快又严。

## 21. record/replay 确定性基建（v1.4.x 核心）

> 面对"回归假阴性/假阳性不可信"的工程问题，用录放给回归"照镜子"。三档分工：
> P1 icount 钉**内核执行**确定性（同输入同输出）→ P2 transcript 录**输入输出流**为可归档证据 →
> P3 回放**消费证据**驱动内核证 bug 表现。它自己是"测试的测试"。

### P1 icount 确定性启动验收（`make test-det`，v1.4.1）

- `tests/test_determinism.sh`：两次 QEMU `-icount shift=auto,align=on,sleep=on` 冷启动，串口
  日志**逐字节 diff** 判定确定性——定时器/中断/调度/网络握手同输入同输出。实测铁证：icount 下
  启动段含 **DHCP OFFER/ACK 握手**两次运行逐字节一致。
- **诚实边界**：交互回归脚本（HMP sendkey / serial / persist / cc500）基于 host 墙钟轮询
  （`wait_for`/`sleep`），与 icount 虚拟时钟流速不匹配 → icount 下 run 窗内超时误报。
  **不回编**这些脚本（其语义是"墙钟真实交互"），icount 确定性独立收编为 `test-det`。

### P2 transcript 固化（`make test-tr`，v1.4.2）

- `tests/transcript.sh`（录制内核，`source` 用）：`tr_start/tr_send/tr_snapshot/tr_abort/
  tr_finish`。`*.in.tr` 列为 **seq / 相对毫秒 / 命令**（TSV，可重放可审计），`*.out.tr` 原始
  字节流，`RESULT` 标 PASS/FAIL 及失败点。
- `tests/test_transcript.sh` 验收三连：① 成功固化产物完整；② **失败自动归档**（`tr_abort`
  固化现场标 FAIL，"人为触发失败可得可复现 transcript"）；③ 复现性雏形（两次冷启同命令集，
  里程碑语义行逐字节一致）。
- **诚实发现**：非 icount 两次运行 `Hello ticks=296/297` 差 1——guest tick 随墙钟调度浮动，
  印证 roadmap"公共时钟须用 icount 虚拟时钟、非 guest tick"。复现性按 `ticks=N` pin 掉噪音，
  真逐字节确定性交给 P1。

### P3 replay 回放差分闭环（`make test-rp`，v1.4.3）

- `tests/replay.sh`（回放器）：`replay_into <in.tr> <out.log> <runid> <done_regex>` 按
  seq/rel_ms/payload 打拍注入串口、等完成信号驱动真实内核路径。**不用"日志静止"作结束判据**
  （本内核有后台 demo 应用持续打印，日志永不静止）。
- `tests/test_replay.sh`：bug 本质闭环——从 bugs.md 抽 **BUG-026**（cc500 形参列表 EOF 未闭合
  →死循环），录 `writefile 写 int main(int x` + `ccrun` 的 transcript → 回放 → 修复版见
  `cc500: error at`（exit(1)，不死循环）。
- **诚实发现**（P3 实测边界，已规避）：icount(TCG) 下 cc500 编译分钟级 + 后台 demo 抢 tick →
  回放不用 icount，bug 闭环靠信号断言（逐字节确定性由 P1 test-det 承担）；跨独立冷启里程碑
  一致不机械稳定 → 两遍一致性作 `REPLAY_VERIFY=1` soft 检查。

### 防御与加固（v1.4.4~v1.4.7）

- **无网络路径 `-nic none`**（v1.4.4）：回放/编译/录制三条不需要网络的路径统一去默认网卡，
  消除启动期 e1000/DHCP 墙钟等待 + 少一个非确定源。内核无网卡优雅跳过（`[net] e1000 not
  found on PCI` + selftest skipped）。理由澄清：icount 下 cc500 慢是用户态 demo 抢指令预算，
  非网卡导致。
- **sqlite 旁路分析索引**（v1.4.4，`tests/tr2sqlite.py`）：`.tr` 文本是**证据原件**，sqlite 是
  只读"放大镜"（三表 transcripts/in_events/out_rows，幂等 DELETE+INSERT 按 runid）——坏了
  绝不影响录放正确性，跨 runid 检索（命令直方图/FAIL 血统/LIKE 编译结果行）不走文本扫描。
- **TSV 守卫**（v1.4.6）：`tr_send` 检测 payload 含原始 TAB/换行即拒绝（不发送、不记录、
  rc=1）并 `%q` 报违规 + 规约提示；文件头加规约行（payload 须单行，多行须显式编码
  `\t\n\\`）——归档 TSV 永不脱列，坏证据不深入。
- **repro_bugs 接录放**（v1.4.5）：BUG-A/BUG-B 复现命令流经 transcript 录制固化为
  `.in.tr/.out.tr`（补上"repro_bugs.sh 只脚本化、未录时间关系"缺口），`send()` 改调 `tr_send`
  （wait 驱动的实际打拍也固化真实相对 ms）；`make test-repro` 独立目标（不在 `test:` 聚合内，
  语义=首方复现/回归）。
- **rp_torture 实战**（v1.4.7，`tests/rp_torture.sh`）：A 两轮 `-icount` 冷启功能契约差分 ·
  B 内核致命/越权/溢出标记扫描（区分预期 procCrash 隔离演示）· C tr2sqlite 检索 · D 重放黄金
  transcript 做结果集复原。初测抓出**两处测试壳"假绿"缺陷**（非内核）：
  - **空判据假绿**：`func()` 未把 `$1` 传给 grep → 读空 stdin → 确定性判定比对两个空文件，
    无条件 PASS。修复后判定真实落到输出文件。
  - **打点节奏缺陷**：26 条连续 `tr_send` 无间隔灌入 + 末尾盲 `sleep 3` 把末条命令掐在半路。
    改为**提示符同步打点** `tsend`（每条等下一个 `mini-os$ ` 再发，计数自增）。
  - **结果集复原语义**：跨打点路径叠加并发 demo 调度，尾部两行偶发对调 → GO/NO-GO 改用
    **结果集相等（排序后）**，顺序差单列已知边界提示，不把顺序噪声误报为回归。
  修复后两轮 `-icount` 冷启确定性成立（out_lines 均精确 =1165 行）、内核无意外缺陷标记、
  现场可复原——"用录放基因为测试本身照镜子"。

## 22. 并发模型不变量（v1.5 审计固化）

> 由来：外部评审（`history/external-reviews/mini-os-arch-and-quality-review_6ac70e4.md`，
> "驱动层锁"follow-up）指出——本内核并发正确性靠**未文档化的隐式约定**维系（单核 + 全中断门
> IF=0 + e1000 设备中断永不使能）。当前无活跃竞态，但契约必须显式化，否则任何一条被无意打破
> 都会静默引入数据竞争。本节把隐式约定固化为不变量，零代码成本。

- **I1 单 vCPU**：QEMU 默认单核（无 SMP 启动路径、无多处理器内存序问题）。所有"并发"都是
  单核上的交错，不存在真并行。
- **I2 门纪律（内核态默认 IF=0）**：全部 IDT 入口为中断门——异常/IRQ 用 `0x8E`、syscall
  （0x80）用 `0xEE` → 进入内核即自动 `cli`，内核态默认 IF=0。推论：**任意时刻至多一个内核
  执行上下文**持有 CPU——ISR 与 syscall 不并发、ISR 与 ISR 不嵌套（中断门自动清 IF），共享
  内核状态的互斥由门纪律免费提供。IF=1 只出现在显式开中断的窗口（见 I3 uart 行）。
- **I3 共享状态归属表**：
  | 共享状态 | 消费者 | 互斥机制 |
  |---|---|---|
  | e1000 rx ring | IRQ0-timer `dhcp_tick` 与 syscall netif 收发 | I2：timer ISR 运行时不进 syscall，反之亦然 |
  | uart 输出行 | IRQ / syscall / IF=1 窗口内任意上下文 | 整行输出须 **xirq 原子化**（`nl_*` 原子行缓冲一次 flush；K1 = 全内核唯一锁原语，为串口行撕裂 BUG-042 而设） |
  | PID / socket / fd 表 | 各 syscall + terminate_current 回收 | I2 + "谁打开谁所有、退出必归还"（v0.31 收口） |
- **I4 演进禁令**：**使能 e1000 设备中断（真异步）或把 trap gate 改回普通门之前，必须先引入
  真锁**（自旋锁或等价协议）。否则 I2 的互斥担保失效、I3 归属表被异步打断——e1000 rx ring
  会从"两消费者串行"退化为"真并发"。当前设备中断永不使能是本模型的前提，不是待办缺陷。
- **与回归体系的关系**：v0.33 的 F-4（selftest 行撕裂）修复正是 I3 中 uart 行原子性的落实；
  `usermode.c:710/722` exec 错误路径"先 `sti` 后 `serial_printf`"（建议：先打印后 sti，
  消除 IF=1 打印窗口）是本节追踪的待办项。

## 23. 文档覆盖对照（治理自证）

| design.md 章 | 覆盖版本 | 说明 |
|---|---|---|
| 1~9 | v0.5~v0.11 | 内核主体（sched/IPC/fs/elf/shell/串口/地址空间） |
| 10 | v0.16~v0.18 | 测试策略五层 + 单行自检 |
| 11~16 | v0.12~v0.17 | 进程/驱动/存储/syscall 边界/设计取舍 |
| 17 | v0.18~v1.3 | 网络全链（e1000→UDP→DHCP→netif v1.1→虚拟 TCP 路标） |
| 18 | v0.26 | 容量三连 |
| 19 | v0.27~v0.27b | 工具链与自举 |
| 20 | v0.29~v0.33 | 加固与工程化收口 |
| 21 | v1.4~v1.4.7 | record/replay 确定性基建 |
| 22 | v1.5 审计 | 并发模型不变量（横切全部版本） |

> 版本演进的事实主源是 `changelog.md`（逐版本）与 `bugs.md`（逐 BUG）；`design.md` 只记
> **跨版本的设计主线与决策理由**，不复制 changelog 流水。后续新版本按维护规则"新子系统
> / 新架构决策 → 加章（带版本戳）"增量维护，避免再次出现"落后 N 版"。
