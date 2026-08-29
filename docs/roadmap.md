# 演进路线（Roadmap）

> 目标：以"最小可用、逐步增量"的方式，把一台裸机从零变成一个
> 具备多任务、文件系统、可执行程序加载的微型操作系统。

## 已完成里程碑

| 版本 | 主题 | 关键成果 |
|------|------|----------|
| v0.1 | 引导 | 软盘引导进入保护模式，VGA 打印 "Hello Micro-OS!" |
| v0.2 | C 内核地基 | multiboot 引导、GDT/IDT、8259 PIC、PIT 定时器(100Hz)、键盘、VGA/串口、异常处理 |
| v0.3 | 内存管理 | 物理页帧分配器、分页、内核堆 kmalloc/kfree、懒分配（缺页按需映射） |
| v0.4 | 用户态 | 重建 GDT(ring3 段)、TSS、int 0x80 系统调用、ring3 用户程序、内存保护（越权写触发页错误） |
| v0.5 | 进程调度 | PCB、抢占式轮转(RR)、sleep/阻塞/唤醒、yield、exit/kill、僵尸回收、内核 idle 进程、宿主单测 + QEMU 回归 |
| v0.6 | IPC 与同步 | 信号量（计数 + 等待队列 + 阻塞/唤醒）、互斥锁、共享内存页、rendezvous 会合演示、宿主单测 + QEMU 回归 |
| v0.7 | IPC：消息队列 | 有界消息队列（环形缓冲 + 双等待队列 + 暂存消息）、阻塞式 send/recv、生产者-消费者演示、宿主单测 + QEMU 回归 |
| v0.8 | 文件系统 | 块设备抽象（内存盘后端）、类 Unix 极简 mini-fs（超级块/位图/inode 表/目录）、文件系统调用（create/open/read/write/close/ls/delete）、procFSA/procFSB 演示、宿主单测 + QEMU 回归 |
| v0.9 | 可执行程序加载与 Shell | ELF32 加载器（PT_LOAD/按链接地址/bss 清零）、应用独立编译并整体内嵌为 initramfs、常驻交互式 shell（help/ls/cat/run/exit）、`run <prog>` 动态加载 ELF 应用、阻塞式 readline/等待子进程退出、键盘行缓冲、宿主单测（kb/elf）+ QEMU 交互式注入回归 |
| v0.10 | 串口终端 | 串口 COM1 接收通道（IRQ4）、`kb_feed_char` 统一键盘/串口输入源、`qemu -serial stdio` 即可交互、`tests/test_serial.sh` 以 FIFO 模拟外部 agent 经串口驱动 shell 的端到端回归 |
| v0.11 | 每进程地址空间与内存隔离 | 每进程独立页目录（`addr_space_create/destroy` + 调度切 CR3）、ELF 加载直写目标地址空间、`sys_map_page` 私有页申请、共享内存按进程重映射、`isol` 应用演示同一虚拟地址落到不同物理页（物理内存隔离） |
| v0.12 | fork/exec 进程模型与 argv | `sys_fork`（地址空间深拷贝 + 共享内存共享）、`sys_exec`（镜像替换 + cdecl argv 布置）、pid 槽位重用（alloc_pid）、应用入口统一 `app_main(argc, argv)`、`forkdemo`（fork 隔离）+ `args`（argv 打印）演示、shell `exec` 命令（fork+exec+argv+wait 全链路） |
| v0.13 | 用户栈守卫页与栈溢出检测 | 每进程 8KB 栈槽 = [守卫页 4KB(不映射) \| 栈页 4KB(映射)]、`stack_guard_hit` 纯逻辑判定（可宿主单测）、pf_handler 识别栈溢出并隔离终止、`stackovf` 演示（写守卫页被内核抓）、SHMEM 区后移避让；Git 仓库建立（v0.12 基线 `ac80cc9`） |
| v0.14 | 文件系统增强 | 目录层级（mkdir/rmdir + 绝对路径解析器，支持 `.`/`..`/重复斜杠）、间接块（单文件 48KB → ~4.1MB，删除递归释放）、文件偏移定位与追加写（`sys_fs_seek`、open mode=2）、`sys_fs_ls` 按路径并按类型打印、shell 新增 `mkdir/rmdir/rm`、`fsdemo` 演示（子目录/追加/seek/100KB 间接块大文件）；修复 `sys_wait` spawn-wait 竞态（BUG-014） |
| v0.15 | 补全 wait 语义与孤儿清理 | `sys_wait(pid,*status)` 升级经典 wait/waitpid（`pid=-1` 等任意子进程、返回 pid、退出码走 status 出参、只回收自己的子进程）；父进程退出时子进程孤儿化（防 pid 槽复用后的孤儿泄漏）；`waitdemo` 演示（fork 3 子不同退出码，`wait(-1)` 依次回收 + verify + 无子返回 -1）；shell run/exec 适配；`exec <不存在程序>` 失败反馈用例 |
| v0.16 | 用户态 CRT 收口 + ATA 真盘持久化 + 单行自检 | ① CRT 收口：ELF 入口改为 `_start`（app_main 返回即 `sys_exit(0)`），根治 fsdemo 类"忘写 sys_exit 栈顶 ret 崩溃"（BUG-016，guard.c 同时改为按 pid 判定守卫页）；② ATA PIO 驱动（LBA28 轮询）+ 存储子系统：真盘整盘读入 ramdisk、magic 有效即挂载（用户数据跨重启存活）、`save` 命令全量写回；③ shell `selftest` 单行结构化自检（`[selftest] PASS (5 checks)`）；回归升级四层（+`tests/test_persist.sh` 两次 QEMU 共享镜像验证持久化） |

## 下一步规划

按依赖顺序演进，两条支线可选：

### 支线 A：继续做深 x86 内核（v0.16+）
- ~~用户栈守卫页（guard page）与栈溢出检测~~ ✅ v0.13 已完成
- ~~更完整的文件系统（目录层级、文件偏移定位/追加写、间接块）~~ ✅ v0.14 已完成
- ~~补全 fork/exec 的 wait 语义（wait/waitpid、孤儿清理）~~ ✅ v0.15 已完成
- ~~用户态 CRT 收口（app_main 返回即 exit）+ ATA 真盘持久化 + 单行自检~~ ✅ v0.16 已完成
- 候选下一步（按价值排序）：
  - **syscall 边界校验（copyin/copyout）**：审计所有 syscall 用户指针，加内核/用户边界校验，防恶意应用传内核地址破坏内核/磁盘镜像（与持久化配套的安全加固）
  - 网络：NIC 驱动（e1000）+ 极简协议栈（TCP/IP 或先 UDP）
  - 真实硬件引导（GRUB/ISO）——串口终端（v0.10）已就绪，届时可直接在真机串口上交互调试
  - 可选：多级间接块/索引节点、mmap/写时复制(COW) fork、信号与信号处理、进程槽扩容

### 支线 B：为移植 ARM 预留架构（HAL 抽象层）
- 抽出 **HAL**（硬件抽象层）：把 GDT/IDT/PIC/PIT/串口/键盘等 x86 特有操作封装成
  `hal_*` 接口，内核其余部分只依赖 HAL
- 地址空间抽象：把"页表/线性地址"抽象为 `vm_space`，隔离 x86 分页细节
- 上下文切换抽象：把 `isr.s` 的寄存器现场/切换路径抽象为架构相关汇编接口
- 目标：换 CPU 时只重写 HAL + 少量汇编 + 链接脚本，调度/内存/文件系统/IPC 全复用
- 风险提示：这是较大重构，**届时必须在独立分支上做**（如 `git switch -c feature/hal`），
  HAL 落地 + 回归全绿后再合并回主线，避免破坏 x86 主线可运行状态

> 建议顺序：先按支线 A 把 x86 内核做扎实（v0.16 ATA 持久化 + CRT 收口已完成，
> 建议继续 v0.17 syscall 边界校验，再考虑网络），
> 等 x86 特性攒够、且真有 ARM 目标时再启动 HAL 重构——因为独立地址空间已牵动页表/切换，
> 届时抽象 HAL 收益最大、返工最少。HAL 属破坏性重构，需开分支。

## 开发原则

1. **每步可运行**：任何一次改动后 `make test` 必须全绿（宿主单测 + QEMU 回归）。
2. **纯逻辑可单测**：与硬件解耦的策略（堆、键盘映射、调度队列）抽成无内核依赖模块，用宿主单测覆盖。
3. **先跑通再优化**：优先正确的功能，再谈性能与安全。
4. **文档随代码走**：每个版本同步更新 changelog / bugs / design。
