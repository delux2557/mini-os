# 演进路线（Roadmap）

> 目标：以"最小可用、逐步增量"的方式，把一台裸机从零变成一个
> 具备多任务、文件系统、可执行程序加载的微型操作系统。

## 已完成里程碑

| 版本    | 主题                            | 关键成果                                                                                                                                                                                                                                                                                                                                               |
| ----- | ----------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| v0.1  | 引导                            | 软盘引导进入保护模式，VGA 打印 "Hello Micro-OS!"                                                                                                                                                                                                                                                                                                                |
| v0.2  | C 内核地基                        | multiboot 引导、GDT/IDT、8259 PIC、PIT 定时器(100Hz)、键盘、VGA/串口、异常处理                                                                                                                                                                                                                                                                                        |
| v0.3  | 内存管理                          | 物理页帧分配器、分页、内核堆 kmalloc/kfree、懒分配（缺页按需映射）                                                                                                                                                                                                                                                                                                           |
| v0.4  | 用户态                           | 重建 GDT(ring3 段)、TSS、int 0x80 系统调用、ring3 用户程序、内存保护（越权写触发页错误）                                                                                                                                                                                                                                                                                        |
| v0.5  | 进程调度                          | PCB、抢占式轮转(RR)、sleep/阻塞/唤醒、yield、exit/kill、僵尸回收、内核 idle 进程、宿主单测 + QEMU 回归                                                                                                                                                                                                                                                                           |
| v0.6  | IPC 与同步                       | 信号量（计数 + 等待队列 + 阻塞/唤醒）、互斥锁、共享内存页、rendezvous 会合演示、宿主单测 + QEMU 回归                                                                                                                                                                                                                                                                                    |
| v0.7  | IPC：消息队列                      | 有界消息队列（环形缓冲 + 双等待队列 + 暂存消息）、阻塞式 send/recv、生产者-消费者演示、宿主单测 + QEMU 回归                                                                                                                                                                                                                                                                                 |
| v0.8  | 文件系统                          | 块设备抽象（内存盘后端）、类 Unix 极简 mini-fs（超级块/位图/inode 表/目录）、文件系统调用（create/open/read/write/close/ls/delete）、procFSA/procFSB 演示、宿主单测 + QEMU 回归                                                                                                                                                                                                                 |
| v0.9  | 可执行程序加载与 Shell                | ELF32 加载器（PT\_LOAD/按链接地址/bss 清零）、应用独立编译并整体内嵌为 initramfs、常驻交互式 shell（help/ls/cat/run/exit）、`run <prog>` 动态加载 ELF 应用、阻塞式 readline/等待子进程退出、键盘行缓冲、宿主单测（kb/elf）+ QEMU 交互式注入回归                                                                                                                                                                           |
| v0.10 | 串口终端                          | 串口 COM1 接收通道（IRQ4）、`kb_feed_char` 统一键盘/串口输入源、`qemu -serial stdio` 即可交互、`tests/test_serial.sh` 以 FIFO 模拟外部 agent 经串口驱动 shell 的端到端回归                                                                                                                                                                                                                 |
| v0.11 | 每进程地址空间与内存隔离                  | 每进程独立页目录（`addr_space_create/destroy` + 调度切 CR3）、ELF 加载直写目标地址空间、`sys_map_page` 私有页申请、共享内存按进程重映射、`isol` 应用演示同一虚拟地址落到不同物理页（物理内存隔离）                                                                                                                                                                                                                    |
| v0.12 | fork/exec 进程模型与 argv          | `sys_fork`（地址空间深拷贝 + 共享内存共享）、`sys_exec`（镜像替换 + cdecl argv 布置）、pid 槽位重用（alloc\_pid）、应用入口统一 `app_main(argc, argv)`、`forkdemo`（fork 隔离）+ `args`（argv 打印）演示、shell `exec` 命令（fork+exec+argv+wait 全链路）                                                                                                                                                   |
| v0.13 | 用户栈守卫页与栈溢出检测                  | 每进程 8KB 栈槽 = \[守卫页 4KB(不映射) \| 栈页 4KB(映射)]、`stack_guard_hit` 纯逻辑判定（可宿主单测）、pf\_handler 识别栈溢出并隔离终止、`stackovf` 演示（写守卫页被内核抓）、SHMEM 区后移避让；Git 仓库建立（v0.12 基线 `ac80cc9`）                                                                                                                                                                                  |
| v0.14 | 文件系统增强                        | 目录层级（mkdir/rmdir + 绝对路径解析器，支持 `.`/`..`/重复斜杠）、间接块（单文件 48KB → \~4.1MB，删除递归释放）、文件偏移定位与追加写（`sys_fs_seek`、open mode=2）、`sys_fs_ls` 按路径并按类型打印、shell 新增 `mkdir/rmdir/rm`、`fsdemo` 演示（子目录/追加/seek/100KB 间接块大文件）；修复 `sys_wait` spawn-wait 竞态（BUG-014）                                                                                                       |
| v0.15 | 补全 wait 语义与孤儿清理               | `sys_wait(pid,*status)` 升级经典 wait/waitpid（`pid=-1` 等任意子进程、返回 pid、退出码走 status 出参、只回收自己的子进程）；父进程退出时子进程孤儿化（防 pid 槽复用后的孤儿泄漏）；`waitdemo` 演示（fork 3 子不同退出码，`wait(-1)` 依次回收 + verify + 无子返回 -1）；shell run/exec 适配；`exec <不存在程序>` 失败反馈用例                                                                                                                   |
| v0.16 | 用户态 CRT 收口 + ATA 真盘持久化 + 单行自检 | ① CRT 收口：ELF 入口改为 `_start`（app\_main 返回即 `sys_exit(0)`），根治 fsdemo 类"忘写 sys\_exit 栈顶 ret 崩溃"（BUG-016，guard.c 同时改为按 pid 判定守卫页）；② ATA PIO 驱动（LBA28 轮询）+ 存储子系统：真盘整盘读入 ramdisk、magic 有效即挂载（用户数据跨重启存活）、`save` 命令全量写回；③ shell `selftest` 单行结构化自检（`[selftest] PASS (5 checks)`）；回归升级四层（+`tests/test_persist.sh` 两次 QEMU 共享镜像验证持久化）                         |
| v0.17 | syscall 边界校验（copyin/copyout）  | `userptr.c/h` 校验层（`user_ptr_valid`/`copyin`/`copyout`/`copyin_str`，纯逻辑可宿主单测）；全部涉用户指针的 syscall（print/readline/spawn/wait/exec/FS 全链路）先校验再使用；`abuse` 演示应用（内核低地址/回绕地址全部被拒、合法路径正常）；宿主单测 test\_userptr + serial/qemu 双通道 `run abuse` 回归                                                                                                                 |
| v0.18 | 网络：e1000 + 极简协议栈              | PCI type-1 配置空间（枚举 + BAR 自分配 + MEM/BUSMASTER）；e1000 驱动（MMIO + 描述符环轮询收发 + ARP 自检）；极简以太网/ARP 帧（netutil 纯逻辑 44 断言）；QEMU 与 SLIRP 网关端到端 ARP 交换 + pcap 独立核验；回归升级五层（+`make test-net`）                                                                                                                                                                     |
| v0.19 | 网络加厚：极简 IP/UDP                | IPv4 头构建/解析 + RFC1071 校验和（ip.c）、UDP 帧构建/解析 + 伪头校验和（udp.c），均纯逻辑可宿主单测（test\_ip/test\_udp）；内核态 e1000 UDP 回环自检（经 SLIRP 到宿主 UDP echo 服务 PING/PONG）；pcap 独立核验线上确有 IPv4/UDP 双向包                                                                                                                                                                           |
| v0.20 | 网络可用：用户态 UDP socket           | `sys_net_socket/sendto/recvfrom/close`（30-33）；内核 `netsock` socket 表 + 网卡轮询分发（recv 先排空 NIC 再取队首，非阻塞与轮询驱动对齐）；`netio.h` 共享 iov 结构（3 参 syscall 承载多参，ABI 与内核一致）；`sockdemo` 用户态端到端回环（socket→sendto PING→轮询 recvfrom PONG）；修复 e1000 MMIO 位于高地址（PDE≥512）、进程页目录只克隆低 1GB PDE 导致的 syscall 路径缺页（netsock 收发前临时切内核页目录）；test\_net 升级六层（+sockdemo 断言 + pcap UDP≥4） |
| v0.21 | 内核自审计 + syscall 边界契约化         | 运行时自审计内建进内核：`sem_invariant_ok`（count+waiters 守恒）、`mem_audit`（used\_frames 与帧位图配平）、`sched_audit`（PCB 状态机合法性），由 syscall 34 `SYS_KERN_AUDIT` 一键触发；selftest 追加第 6 项，`[selftest] PASS (6 checks)` 从"5 个应用没崩"升级为"内核核心不变量成立"；调度日志（block/wake/exit）统一带单调 tick 戳；abuse 边界断言补齐至 17 项（exec argv / sendto·recvfrom iov / read·write buf 等内核地址一律 -1）            |
| v0.22 | 网络交互化：shell `netping` 命令      | shell 内建 `netping [ip] [port]`（默认 10.0.2.2:7777）：开 UDP socket 发 PING、轮询收 PONG，单行原子打印 `[netping] <ip>:<port> PONG +<N>B rtt=<T> ticks`（IP 大端序正确显示）；把"演示程序"升级为"交互命令"，agent 可在会话中一键验证网络连通性；test\_net 六层 + HMP sendkey 交互注入 netping 断言（pcap UDP 4→6）                                                                                                   |
| v0.23 | ICMP Echo：PING 通宿主            | `net/icmp.c/h` 纯逻辑（Ethernet+IPv4+ICMP Echo 请求/应答，校验和只覆盖 ICMP 报文 RFC 792、无伪头）+ 宿主单测 test\_icmp（22 断言）；`e1000_icmp_selftest` 开机自检：发 Echo 请求到 SLIRP 网关 10.0.2.2，收其回显应答（`[icmp] echo reply from 10.0.2.2 OK (rtt=N ticks)`）；test\_net 串口断言 + pcap 独立核验 IPv4/ICMP 双向 ≥2；补上"ping 即网络活"的经典语义                                                              |
| v0.24 | UDP 校验和错误路径                   | `udp_parse` 接收端校验 UDP 校验和（RFC 768：伪头+UDP 头+载荷重算须折叠为 0），坏包一律拒绝、netsock 分发据此静默丢包；校验和字段 0 = 发送端未计算 → 接受，发送端算得 0 以 0xFFFF 发送（两者不混淆）；宿主单测 test\_udp 追加 6 条（24→30，载荷/校验和字段/伪头 srcIP 篡改全拒、=0 接受）；test\_net 全绿（真实 SLIRP PONG 校验和有效不受影响）                                                                                                                    |
| v0.25 | DHCP 客户端：动态获取 IP/网关           | `net/dhcp.c/h` 纯逻辑（BOOTP 固定头+选项，RFC 2131/2132）+ 宿主单测 test\_dhcp（38 断言）；`e1000_dhcp_run` 开机四步状态机（DISCOVER→OFFER→REQUEST→ACK，忙等超时 ~2s、NAK/超时重试），失败回退静态——静态兜底收敛为单一配置点 `NET_STATIC_IP`/`NET_STATIC_GW`；`e1000_my_ip()`/`e1000_gw_ip()` 访问器，ARP/UDP/ICMP 三自检改取动态 IP；test\_net 新增 DHCP 四项断言全绿                                                            |
| v0.26 | 容量三连#1：用户栈按需生长               | 每进程栈槽 8KB 固定 → 32KB（槽底硬底守卫页 4K 永不映射 + 28KB 可生长栈区，栈顶页起、守卫页随栈底下移）；`stack_guard_hit` 二态扩三态（OK/GROWTH/BOOM，纯逻辑可宿主单测）；`pf_handler` 命中守卫页补映射新栈页并更新 PCB 记账、深越界/到硬底才判溢出；PCB `stack_frame` 改 `stack_frames[]`+`stack_fcount`+`stack_bottom`，`stack_init`/`stack_free` 统一管理；地址空间重布局（栈区 0x80010000-0x80090000，shell/app 槽/SHMEM 后移）；`deep` 演示 12KB 递归触发 3 次生长；宿主 34 断言 + QEMU/串口回归全绿 |

## 下一步规划

按依赖顺序演进，两条支线可选：

### 支线 A：继续做深 x86 内核（v0.16+）

* ~~用户栈守卫页（guard page）与栈溢出检测~~ ✅ v0.13 已完成

* ~~更完整的文件系统（目录层级、文件偏移定位/追加写、间接块）~~ ✅ v0.14 已完成

* ~~补全 fork/exec 的 wait 语义（wait/waitpid、孤儿清理）~~ ✅ v0.15 已完成

* ~~用户态 CRT 收口（app\_main 返回即 exit）+ ATA 真盘持久化 + 单行自检~~ ✅ v0.16 已完成

* ~~syscall 边界校验（copyin/copyout）~~ ✅ v0.17 已完成

* ~~网络：e1000 驱动 + 极简协议栈（ARP，QEMU/SLIRP 端到端）~~ ✅ v0.18 已完成

* ~~网络加厚：极简 IP/UDP（纯逻辑可宿主单测）~~ ✅ v0.19 已完成

* ~~用户态 UDP socket（sys\_net\_\* + sockdemo 端到端回环）~~ ✅ v0.20 已完成

* ~~内核自审计（不变量检查）+ syscall 边界契约化~~ ✅ v0.21 已完成

* ~~socket 演示可交互化（shell~~ ~~`netping`~~ ~~命令）~~ ✅ v0.22 已完成

* ~~ICMP 回显（PING 通宿主）~~ ✅ v0.23 已完成

* ~~UDP 校验和错误路径~~ ✅ v0.24 已完成

* ~~DHCP 静态 IP 可配置化（动态获取，失败回退单一静态配置点）~~ ✅ v0.25 已完成

* 候选下一步（按价值排序）：

  * **网络进一步可用化**：DHCP 租期续约（T1/T2 定时 renew）；
    TCP 状态机暂缓（复杂度高，非"最小可演进"核心）

  * 真实硬件引导（GRUB/ISO）——串口终端（v0.10）已就绪，届时可直接在真机串口上交互调试；
    注意真机网卡/磁盘与模拟器不同（非 82540EM / 多为 AHCI），网络与持久化验证以模拟器为准

  * 可选：多级间接块/索引节点、mmap/写时复制(COW) fork、信号与信号处理、进程槽扩容

### 支线 B：为移植 ARM 预留架构（HAL 抽象层）

* 抽出 **HAL**（硬件抽象层）：把 GDT/IDT/PIC/PIT/串口/键盘等 x86 特有操作封装成
  `hal_*` 接口，内核其余部分只依赖 HAL

* 地址空间抽象：把"页表/线性地址"抽象为 `vm_space`，隔离 x86 分页细节

* 上下文切换抽象：把 `isr.s` 的寄存器现场/切换路径抽象为架构相关汇编接口

* 目标：换 CPU 时只重写 HAL + 少量汇编 + 链接脚本，调度/内存/文件系统/IPC 全复用

* 风险提示：这是较大重构，**届时必须在独立分支上做**（如 `git switch -c feature/hal`），
  HAL 落地 + 回归全绿后再合并回主线，避免破坏 x86 主线可运行状态

> 建议顺序：优先支线 C 把"agent 在 guest 内写-编-跑闭环"立起来——
> 先做 v0.26 容量三连（栈按需生长 / sys\_brk 用户堆 / ELF 加载去上限，全纯扩展、可宿主单测），
> 再按 27a/27b → 28 → 29 拆小推进工具链与自举（汇编器+链接器 → C 前端 → libc/crt0 → 自举仪式）。
> 此间 v0.23-v0.25 网络加厚（ICMP / UDP 校验和 / DHCP 动态取 IP）已先后完成，
> 网络已从"驱动"做到"可交互验证 + 经典 ping + 坏包防线 + 动态地址配置"。
> P2 并行可插：DHCP 租期续约、record/replay（QEMU -icount reverse-debug，勿引 rr）。
> 真机 GRUB/ISO 冒烟在工具链落地后再做（以模拟器验证驱动为准）。
> 等 x86 特性攒够、且真有 ARM 目标时再启动 HAL 重构——因为独立地址空间已牵动页表/切换，
> 届时抽象 HAL 收益最大、返工最少。HAL 属破坏性重构，需开分支。

### 支线 C（采纳评估简报·新主线）：agent 在 guest 内完成「写-编-跑」闭环

> **战略依据**：v0.17 copyin/copyout 的真正意义 = "敢让 agent 运行任意编译产物"的安全前提
> （已在）；运行任意程序后，内核 `SYS_KERN_AUDIT`（v0.21）即裁判。把"agent 维护的内核"
> 升级为"agent 能在里面干活的世界"，是测评体系的终极任务形态、教学链终点章、真机叙事收官。
> 现状盘点（均已对代码核实）：写文件✅ FS syscall 齐备但 shell 缺 `writefile`/重定向；
> 编译❌ guest 内无编译器；运行✅ 但被三处容量卡死（见 v0.26）。

* **v0.26「容量三连」（纯扩展，每项都是已验证机制的组合，风险低）**

  * **用户栈按需生长**：~~现每进程 8KB 槽固定（守卫 4K + 栈 4K）。做法：多页槽 + 守卫页随栈
    下移；`stack_guard_hit`（guard.c，现二态 0/1）扩为三态 OK/GROWTH/BOOM，pf\_handler 命中
    "栈区且距当前栈页 1 页以内" → 补映射、守卫页下移；其余维持原判定。
    = v0.3 懒分配 + v0.13 守卫页两个已验证机制的组合。~~ ✅ **v0.26#1 已完成**
    （32KB 槽 = 硬底守卫页 + 28KB 可生长栈区，三态判定，`deep` 演示 12KB 递归触发 3 次生长）

  * **`sys_brk`** **用户堆**：~~现无堆（`map_frames[8]` 固定、`sys_map_page` 一次一页）。在私有页
    之上开可伸缩区，记账进 PCB；记账表 kmalloc 动态化（为编译器 malloc 铺路）。~~ ✅ **v0.26#2 已完成**
    （SYS_BRK 查询/设置 program break，堆区 320KB，扩展按页补映射、收缩保留映射复用，
    `heapdemo` 演示 + `test_brk` 宿主单测）

  * **ELF 加载去上限**：~~`load_frames[APP_MAXFRAMES=8]` 改动态列表、app 区 16KB 扩 MB 级；
    顺带解决 `APP_LINK` 单槽掩护的结构债（并发跑两个同链接地址程序）。~~ ✅ **v0.26#3 已完成**
    （load\_frames/own\_frames 动态化、app 区 1MB、用户空间 16MB；`bigdemo` 70KB/21 帧验证）

  * 三项均可纯逻辑化宿主单测（stack\_guard\_hit 边界、brk 状态机、加载器 frames 记账），
    延续现有测试风格。

* **v0.27-29「工具链与自举」（迄今最大单版本，必须拆小）**

  * **移植对象修正**：简报原指"Rob Pike c5"——核实后 c5 大概率是 8086 16 位版本，与 i386
    32 位平坦模型不匹配，**不直接采用**。更稳妥候选：

    * **cc500**（E. Grimley-Evans，~750 行）：stdin 读 C → stdout 出 **x86-32 ELF**，自托管、
      无 libc 依赖（内置 exit/getchar/malloc/putchar 机器码，malloc 用 brk 实现）——
      与我们的 ELF 加载器 + v0.26 `sys_brk` 天然衔接（GPL-2.0，参考/自写）；

    * 或自写 C 子集前端 + 简单 x86-32 代码生成。

    * 可行性锚点（简报成立）：PWB/C 曾以 56KB 内存在 PDP-11 自举；128MB QEMU + 容量三连后
      属"过于富裕"的尺度。

  * **Micro-OS libc**：`user_lib.h` 已是雏形——补 printf/malloc/brk + open/read/write/exec
    包装 + `crt0`；纯逻辑部分（printf 状态机、malloc 堆算法）复用 heap.c 经验宿主单测。

  * **红线（不要走的路）**：不移植 GCC/clang/TinyCC 完整体、不做动态链接（黑洞，吃掉项目余生）。

  * **版本拆分**：27a 汇编器+链接器跑通（手工输入出可执行 ELF 进 FS、被 `run` 执行）；
    27b C 子集前端 → 全链通；28 libc + crt0 + 若干样例程序；29 自举 + 端到端回归通道。

  * **v0.29 自举仪式验收**：`cc.c` 编译出 `cc2`，`cc2` 再编译 `cc.c`，产物逐字节一致；
    随后演示剧本：agent 经串口/UDP 通道 → `cat > hello.c` → `cc hello.c` → `run a.out`；
    selftest 追加"cc 自举 + 编译循环"两项（沿用 `PASS (N checks)` 行不变式）。

* **P2 并行项（可插在网络收尾之前/之间）**

  * **record/replay 提前**（简报论点成立：v0.20 "e1000 MMIO 高地址 × 页目录只克隆低 1GB"
    类跨子系统 bug 证明组合爆炸已开始，每加子系统它越便宜）。
    **修正**：不引 rr 工具（rr 面向 x86-64 原生，不直接适配 QEMU guest），改
    **QEMU** **`-icount`** **+ gdb reverse-debugging**（TCG i386 原生支持），日志钉点基于
    v0.21 tick 时间戳（已备），失败 transcript 固化回归。

  * **DHCP 租期续约（T1/T2 renew）**：网络收尾。简报"ICMP + 静态 IP 可配"已由 v0.23/v0.25
    完成，此为其剩余项。

## 开发原则

1. **每步可运行**：任何一次改动后 `make test` 必须全绿（宿主单测 + QEMU 回归）。
2. **纯逻辑可单测**：与硬件解耦的策略（堆、键盘映射、调度队列）抽成无内核依赖模块，用宿主单测覆盖。
3. **先跑通再优化**：优先正确的功能，再谈性能与安全。
4. **文档随代码走**：每个版本同步更新 changelog / bugs / design。

