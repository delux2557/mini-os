# 演进路线（Roadmap）

> 目标：以"最小可用、逐步增量"的方式，把一台裸机从零变成一个
> 具备多任务、文件系统、网络、可执行程序加载与**自举能力**的微型操作系统。
> 项目已越过"功能积累期"（教学闭环达成），进入 **"收尾-加固-沉淀"** 阶段——
> 最终交付物 = 可运行内核（五层回归全绿）+ 工程方法论文档 + "AI 能写操作系统"的实证案例。

## 已完成里程碑

| 版本     | 主题                                       | 关键成果                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ------ | ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| v0.1   | 引导                                       | 软盘引导进入保护模式，VGA 打印 "Hello Micro-OS!"                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| v0.2   | C 内核地基                                   | multiboot 引导、GDT/IDT、8259 PIC、PIT 定时器(100Hz)、键盘、VGA/串口、异常处理                                                                                                                                                                                                                                                                                                                                                                                                           |
| v0.3   | 内存管理                                     | 物理页帧分配器、分页、内核堆 kmalloc/kfree、懒分配（缺页按需映射）                                                                                                                                                                                                                                                                                                                                                                                                                              |
| v0.4   | 用户态                                      | 重建 GDT(ring3 段)、TSS、int 0x80 系统调用、ring3 用户程序、内存保护（越权写触发页错误）                                                                                                                                                                                                                                                                                                                                                                                                           |
| v0.5   | 进程调度                                     | PCB、抢占式轮转(RR)、sleep/阻塞/唤醒、yield、exit/kill、僵尸回收、内核 idle 进程、宿主单测 + QEMU 回归                                                                                                                                                                                                                                                                                                                                                                                              |
| v0.6   | IPC 与同步                                  | 信号量（计数 + 等待队列 + 阻塞/唤醒）、互斥锁、共享内存页、rendezvous 会合演示、宿主单测 + QEMU 回归                                                                                                                                                                                                                                                                                                                                                                                                       |
| v0.7   | IPC：消息队列                                 | 有界消息队列（环形缓冲 + 双等待队列 + 暂存消息）、阻塞式 send/recv、生产者-消费者演示、宿主单测 + QEMU 回归                                                                                                                                                                                                                                                                                                                                                                                                    |
| v0.8   | 文件系统                                     | 块设备抽象（内存盘后端）、类 Unix 极简 mini-fs（超级块/位图/inode 表/目录）、文件系统调用（create/open/read/write/close/ls/delete）、procFSA/procFSB 演示、宿主单测 + QEMU 回归                                                                                                                                                                                                                                                                                                                                    |
| v0.9   | 可执行程序加载与 Shell                           | ELF32 加载器（PT\_LOAD/按链接地址/bss 清零）、应用独立编译并整体内嵌为 initramfs、常驻交互式 shell（help/ls/cat/run/exit）、`run <prog>` 动态加载 ELF 应用、阻塞式 readline/等待子进程退出、键盘行缓冲、宿主单测（kb/elf）+ QEMU 交互式注入回归                                                                                                                                                                                                                                                                                              |
| v0.10  | 串口终端                                     | 串口 COM1 接收通道（IRQ4）、`kb_feed_char` 统一键盘/串口输入源、`qemu -serial stdio` 即可交互、`tests/test_serial.sh` 以 FIFO 模拟外部 agent 经串口驱动 shell 的端到端回归                                                                                                                                                                                                                                                                                                                                    |
| v0.11  | 每进程地址空间与内存隔离                             | 每进程独立页目录（`addr_space_create/destroy` + 调度切 CR3）、ELF 加载直写目标地址空间、`sys_map_page` 私有页申请、共享内存按进程重映射、`isol` 应用演示同一虚拟地址落到不同物理页（物理内存隔离）                                                                                                                                                                                                                                                                                                                                       |
| v0.12  | fork/exec 进程模型与 argv                     | `sys_fork`（地址空间深拷贝 + 共享内存共享）、`sys_exec`（镜像替换 + cdecl argv 布置）、pid 槽位重用（alloc\_pid）、应用入口统一 `app_main(argc, argv)`、`forkdemo`（fork 隔离）+ `args`（argv 打印）演示、shell `exec` 命令（fork+exec+argv+wait 全链路）                                                                                                                                                                                                                                                                      |
| v0.13  | 用户栈守卫页与栈溢出检测                             | 每进程 8KB 栈槽 = \[守卫页 4KB(不映射) \| 栈页 4KB(映射)]、`stack_guard_hit` 纯逻辑判定（可宿主单测）、pf\_handler 识别栈溢出并隔离终止、`stackovf` 演示（写守卫页被内核抓）、SHMEM 区后移避让；Git 仓库建立（v0.12 基线 `ac80cc9`）                                                                                                                                                                                                                                                                                                     |
| v0.14  | 文件系统增强                                   | 目录层级（mkdir/rmdir + 绝对路径解析器，支持 `.`/`..`/重复斜杠）、间接块（单文件 48KB → \~4.1MB，删除递归释放）、文件偏移定位与追加写（`sys_fs_seek`、open mode=2）、`sys_fs_ls` 按路径并按类型打印、shell 新增 `mkdir/rmdir/rm`、`fsdemo` 演示（子目录/追加/seek/100KB 间接块大文件）；修复 `sys_wait` spawn-wait 竞态（BUG-014）                                                                                                                                                                                                                          |
| v0.15  | 补全 wait 语义与孤儿清理                          | `sys_wait(pid,*status)` 升级经典 wait/waitpid（`pid=-1` 等任意子进程、返回 pid、退出码走 status 出参、只回收自己的子进程）；父进程退出时子进程孤儿化（防 pid 槽复用后的孤儿泄漏）；`waitdemo` 演示（fork 3 子不同退出码，`wait(-1)` 依次回收 + verify + 无子返回 -1）；shell run/exec 适配；`exec <不存在程序>` 失败反馈用例                                                                                                                                                                                                                                      |
| v0.16  | 用户态 CRT 收口 + ATA 真盘持久化 + 单行自检            | ① CRT 收口：ELF 入口改为 `_start`（app\_main 返回即 `sys_exit(0)`），根治 fsdemo 类"忘写 sys\_exit 栈顶 ret 崩溃"（BUG-016，guard.c 同时改为按 pid 判定守卫页）；② ATA PIO 驱动（LBA28 轮询）+ 存储子系统：真盘整盘读入 ramdisk、magic 有效即挂载（用户数据跨重启存活）、`save` 命令全量写回；③ shell `selftest` 单行结构化自检（`[selftest] PASS (5 checks)`）；回归升级四层（+`tests/test_persist.sh` 两次 QEMU 共享镜像验证持久化）                                                                                                                                            |
| v0.17  | syscall 边界校验（copyin/copyout）             | `userptr.c/h` 校验层（`user_ptr_valid`/`copyin`/`copyout`/`copyin_str`，纯逻辑可宿主单测）；全部涉用户指针的 syscall（print/readline/spawn/wait/exec/FS 全链路）先校验再使用；`abuse` 演示应用（内核低地址/回绕地址全部被拒、合法路径正常）；宿主单测 test\_userptr + serial/qemu 双通道 `run abuse` 回归                                                                                                                                                                                                                                    |
| v0.18  | 网络：e1000 + 极简协议栈                         | PCI type-1 配置空间（枚举 + BAR 自分配 + MEM/BUSMASTER）；e1000 驱动（MMIO + 描述符环轮询收发 + ARP 自检）；极简以太网/ARP 帧（netutil 纯逻辑 44 断言）；QEMU 与 SLIRP 网关端到端 ARP 交换 + pcap 独立核验；回归升级五层（+`make test-net`）                                                                                                                                                                                                                                                                                        |
| v0.19  | 网络加厚：极简 IP/UDP                           | IPv4 头构建/解析 + RFC1071 校验和（ip.c）、UDP 帧构建/解析 + 伪头校验和（udp.c），均纯逻辑可宿主单测（test\_ip/test\_udp）；内核态 e1000 UDP 回环自检（经 SLIRP 到宿主 UDP echo 服务 PING/PONG）；pcap 独立核验线上确有 IPv4/UDP 双向包                                                                                                                                                                                                                                                                                              |
| v0.20  | 网络可用：用户态 UDP socket                      | `sys_net_socket/sendto/recvfrom/close`（30-33）；内核 `netsock` socket 表 + 网卡轮询分发（recv 先排空 NIC 再取队首，非阻塞与轮询驱动对齐）；`netio.h` 共享 iov 结构（3 参 syscall 承载多参，ABI 与内核一致）；`sockdemo` 用户态端到端回环（socket→sendto PING→轮询 recvfrom PONG）；修复 e1000 MMIO 位于高地址（PDE≥512）、进程页目录只克隆低 1GB PDE 导致的 syscall 路径缺页（netsock 收发前临时切内核页目录）；test\_net 升级六层（+sockdemo 断言 + pcap UDP≥4）                                                                                                                    |
| v0.21  | 内核自审计 + syscall 边界契约化                    | 运行时自审计内建进内核：`sem_invariant_ok`（count+waiters 守恒）、`mem_audit`（used\_frames 与帧位图配平）、`sched_audit`（PCB 状态机合法性），由 syscall 34 `SYS_KERN_AUDIT` 一键触发；selftest 追加第 6 项，`[selftest] PASS (6 checks)` 从"5 个应用没崩"升级为"内核核心不变量成立"；调度日志（block/wake/exit）统一带单调 tick 戳；abuse 边界断言补齐至 17 项（exec argv / sendto·recvfrom iov / read·write buf 等内核地址一律 -1）                                                                                                                               |
| v0.22  | 网络交互化：shell `netping` 命令                 | shell 内建 `netping [ip] [port]`（默认 10.0.2.2:7777）：开 UDP socket 发 PING、轮询收 PONG，单行原子打印 `[netping] <ip>:<port> PONG +<N>B rtt=<T> ticks`（IP 大端序正确显示）；把"演示程序"升级为"交互命令"，agent 可在会话中一键验证网络连通性；test\_net 六层 + HMP sendkey 交互注入 netping 断言（pcap UDP 4→6）                                                                                                                                                                                                                      |
| v0.23  | ICMP Echo：PING 通宿主                       | `net/icmp.c/h` 纯逻辑（Ethernet+IPv4+ICMP Echo 请求/应答，校验和只覆盖 ICMP 报文 RFC 792、无伪头）+ 宿主单测 test\_icmp（22 断言）；`e1000_icmp_selftest` 开机自检：发 Echo 请求到 SLIRP 网关 10.0.2.2，收其回显应答（`[icmp] echo reply from 10.0.2.2 OK (rtt=N ticks)`）；test\_net 串口断言 + pcap 独立核验 IPv4/ICMP 双向 ≥2；补上"ping 即网络活"的经典语义                                                                                                                                                                                 |
| v0.24  | UDP 校验和错误路径                              | `udp_parse` 接收端校验 UDP 校验和（RFC 768：伪头+UDP 头+载荷重算须折叠为 0），坏包一律拒绝、netsock 分发据此静默丢包；校验和字段 0 = 发送端未计算 → 接受，发送端算得 0 以 0xFFFF 发送（两者不混淆）；宿主单测 test\_udp 追加 6 条（24→30，载荷/校验和字段/伪头 srcIP 篡改全拒、=0 接受）；test\_net 全绿（真实 SLIRP PONG 校验和有效不受影响）                                                                                                                                                                                                                                       |
| v0.25  | DHCP 客户端：动态获取 IP/网关                      | `net/dhcp.c/h` 纯逻辑（BOOTP 固定头+选项，RFC 2131/2132）+ 宿主单测 test\_dhcp（38 断言）；`e1000_dhcp_run` 开机四步状态机（DISCOVER→OFFER→REQUEST→ACK，忙等超时 \~2s、NAK/超时重试），失败回退静态——静态兜底收敛为单一配置点 `NET_STATIC_IP`/`NET_STATIC_GW`；`e1000_my_ip()`/`e1000_gw_ip()` 访问器，ARP/UDP/ICMP 三自检改取动态 IP；test\_net 新增 DHCP 四项断言全绿                                                                                                                                                                              |
| v0.26  | 容量三连#1：用户栈按需生长                           | 每进程栈槽 8KB 固定 → 32KB（槽底硬底守卫页 4K 永不映射 + 28KB 可生长栈区，栈顶页起、守卫页随栈底下移）；`stack_guard_hit` 二态扩三态（OK/GROWTH/BOOM，纯逻辑可宿主单测）；`pf_handler` 命中守卫页补映射新栈页并更新 PCB 记账、深越界/到硬底才判溢出；PCB `stack_frame` 改 `stack_frames[]`+`stack_fcount`+`stack_bottom`，`stack_init`/`stack_free` 统一管理；地址空间重布局（栈区 0x80010000-0x80090000，shell/app 槽/SHMEM 后移）；`deep` 演示 12KB 递归触发 3 次生长；宿主 34 断言 + QEMU/串口回归全绿                                                                                             |
| v0.27  | 工具链与自举：guest 内「写-编-跑」闭环                  | 移植自托管 C 子集编译器 **cc500**（`tools/cc500/cc500.c`）进 guest：链接基址 0x800A0000 + mini-os ABI；唯一机器码 stub 收敛为通用 `syscall3`（eax/ebx/ecx/edx=int 0x80），exit/malloc/getchar/putchar/sys\_print 全用 C 子集实现（malloc=brk bump）；I/O 走 mini-fs（整读 /cc500.c、编译完写回 /out.elf）；initramfs 嵌入编译器 ELF + 自举源码；shell `ccboot` 命令验证**自举不动点**——gcc 版编译出 P1、P1 再编译出 P2，P1==P2（FNV+字节数逐字节一致，18079B）⇒ 写-编-跑闭环在 guest 内跑通；修复 BUG-025（brk 页中部映射空洞）；回归五层全绿                                                  |
| v0.27b | 写-编-跑演示闭环：cc500 命令行路径 + shell 写/编/跑      | cc500 支持 `argv[1]=输入 argv[2]=输出`（`load_ptr` 逐字节拼 4 字节指针；缺省回退 /cc500.c→/out.elf）；入口/CRT 声明顺序对齐 CC500 反向压参与内核 cdecl 入口的差异；shell 新增 `writefile <path> <content>`（agent 写源码，ARG\_MAX 32→128）+ `ccrun <src> <out>`（fork+exec 编译→运行→校验退出码）；guest 内完整剧本跑通：`writefile /hello.c … → ccrun /hello.c /hello.elf` → 编译产物被加载运行；修复 BUG-026（cc500 对畸形输入死循环，加 EOF 守卫）；页错误日志附 EIP/eax/ebx 便于定位用户态故障；回归五层全绿                                                                             |
| v0.28  | 网络收尾：DHCP 租期续约（T1/T2 renew）              | RFC 2131 §4.4.5 租期续约：`e1000_dhcp_tick()` 由 timer 心跳每 tick 非阻塞驱动（状态机 RENEW\_NONE/SENT、REBIND\_SENT、REACQ\_OFFER/ACK）；到 T1=0.5×lease 单播 RENEW（ciaddr+54+50）、到 T2=0.875×lease 广播 REBIND（仅 50），ACK 重置、NAK/超时重新获取→静态兜底；netsock 注册端口 68 专用 DHCP socket（`netsock_dhcp_open/recv`）解决 sockdemo"排空"网卡抢先消费应答；修复 tick 内用户页目录访问高地址 MMIO 缺页（临时切内核页目录）与 print\_ip 缺 `& 0xFF` 显示 bug；宿主单测 test\_dhcp 38→61；test\_net 短租期（`DHCP_RENEW_SECS=2`）断言 RENEW→ACK 续约闭环（pcap UDP 10→12）；五层回归全绿 |
| v0.29  | 加固：宿主 fuzz + 内核堆审计                       | 宿主侧确定性 fuzz（`tests/fuzz_parse.c`，确定性 PRNG 注入畸形路径/字节，ASan 清洁 60000 轮/36 万次解析调用，覆盖 fs\_walk/elf\_load/net\_\* 解析恶意帧）；内核堆审计 `heap_audit()`（magic/free/size 上界 + used/free 记账对账，防泄漏/双free/破坏块头）并入 kern\_audit；修 BUG-029（icmp 短帧越界读）、BUG-030（fork 子进程继承已生长栈误判缺页）                                                                                                                                                                                                           |
| v0.30  | 修复工具链严重 BUG                              | BUG-031 文件槽泄漏污染工具链（文件槽记打开者 pid + `fs_files_close_pid` 退出回收）、BUG-032 cc500 自编译产物静默丢 argv（入口桩把内核 argc/argv 编组成首函数形参）、代码审查 BUG-033/034/035（页表帧 OOM 写物理0 / kb 行缓冲并发追加 / fork\_frames\[24] 上限）；工程：新增 `repro_bugs.sh` 双断言、版本串单一来源 `src/version.h=MINI_OS_VERSION`（banner/motd/回归断言统一取）、整理 BUG-036（cc500 豁免 `-w`≠压 permerror，GCC14 需 `-Wno-int-conversion`）                                                                                                                  |
| v0.31  | 内核资源归属收口：per-process fd 表 + socket 归属/回收 | ① per-process fd：`fs_files` 全局表迁入 `pcb_t.fd_table[]`，fd 号进程私有，fork 深拷贝/exec/exit 清本表——根治跨进程槽号冲突与异常退出泄漏（v0.30 记 pid 归属方案的彻底收口）；② socket 归属：`net_sock_t` 增 `pid` + `reserved`（DHCP 保留槽），`terminate_current` 回收（F-0a）、close 仅限本进程且禁关保留槽（F-0b）、`netsock_audit` 观测（F-0c）；新增 `test_socket.sh` 攻击回归，六层全绿                                                                                                                                                                     |
| v0.32  | cc500 编译器三缺陷修复                           | F-3 未闭合字符串字面量越界自噬（get\_token EOF 守卫 + primary NUL 守卫）→ `cc500: bad string`；F-2 只声明未定义函数静默编出"call ELF 头"废产物（be\_finish 收尾 'U' 符号检测）→ `cc500: undefined symbol`；F-1 关系运算补齐 `<`/`>`/`>=` + `error()` 带 token 上下文 + `\n`/`\t` 解码；收编 `host_crt.c` hostcc 基座 + `test_cc500.sh`（症状对立断言）挂入 test；自举不动点 P1==P2 保持，六/七层回归全绿                                                                                                                                                      |
| v0.33  | 回归可观测性收口                                 | F-4 selftest 汇总行撕裂（Shell 改 `nl_*` 原子行 + 撕裂探测器）、F-5 pid 表耗尽静默（alloc\_pid 日志 + audit slots）；harness 退出码语义统一（0 全绿/1 断言失败/2 环境缺失，7 脚本前置检查）；CI 全链 `make test` + layers 并行矩阵 + 失败日志工件 + workflow\_dispatch（审核方落地）；账本收口（external-reviews 索引、version bump v0.33）                                                                                                                                                                                                              |

## 下一步规划

> **当前路线以下方三阶段为准**；"支线 A/B/C"为历史规划存档（多数条目已勾选完成，
> 保留作演进记录）。

### 项目阶段判断

| 维度     | 状态                      | 判断        |
| ------ | ----------------------- | --------- |
| 核心概念覆盖 | 进程/内存/文件/网络/工具链全部完成     | 教学闭环已达成   |
| 工程质量   | 五层测试、零告警、纯逻辑可单测、四件套文档   | 工程成熟度高    |
| 代码规模   | \~500KB / 28 个版本        | 接近维护临界点   |
| 自举能力   | guest 内写-编-跑闭环、编译器不动点验证 | 已具备自我演化能力 |

**结论**：继续无限堆砌新子系统边际收益递减，而跨子系统组合的维护成本递增
（BUG-020/025/026 已展示该趋势）。下一步按三阶段推进，不再追逐版本号。

### 阶段一「收尾」（只补欠账，不开新坑）

* ✅ **P0 泄漏修复**（v0.28，BUG-027/028）：`sys_map_page` 记账槽满分配前拒绝；
  exec 路径归还 `load_frames` 数组

* ✅ **DHCP 租期续约**（v0.28，RFC 2131 §4.4.5）：T1 单播 RENEW / T2 广播 REBIND /
  ACK 重置 / NAK·超时重新获取

* ✅ **sys\_map\_page 容量上限显式化**：超 8 页返回 -1 而非静默泄漏（BUG-027 的一部分）

* ✅ **BUG-031 文件槽泄漏**（v0.30，用户实操报告 + 复现）：全局 `fs_files[8]`
  无进程归属、退出路径不清理——一次编译失败（cc500 parse error）即烧掉 slot2、
  污染整条工具链直到重启。修复：槽记打开者 pid，进程退出按归属归还
  （`fs_files_close_pid`）；per-process fd 表（打开文件表入 PCB）仍留作架构债

* ✅ **BUG-032 cc500 入口桩丢 argv**（v0.30，用户实操报告 + 复现）：`be_start` 裸
  `call` 不编组 argc/argv，自编译产物静默吞 argv。修复：入口桩 call 前压 argv/argc
  （与 cc500 "首参 8(%esp)/末参 4(%esp)" 约定对齐）；v0.27b "命令行路径"现对自编译
  产物也成立

* ✅ **`fork_frames`** **动态化**（v0.30，BUG-035，=OBS-002）：`sched_fork` 先数需深拷贝
  页数再按需 kmalloc 动态数组（同 `own_frames`），退出 kfree——大进程（bigdemo 28 页）
  fork 不再受 24 帧硬编码上限限制

* \[可选] **pipe（管道）**：经典 IPC 补全（字节流 vs 消息队列的离散消息，互为补充）。
  ⚠️ 定性为**新功能**而非欠账，与"收尾"原则有张力——若做，归入"如果还想做深"选项

### 阶段二「加固」（不增功能，增信心）

* ✅ **fuzz**（v0.29，`tests/fuzz_parse.c`）：确定性 xorshift32 注入随机路径/随机字节，
  覆盖 `fs_walk` / `elf_load_range` / `net_eth_type` / `arp` / `ip_parse` / `udp_parse` /
  `icmp_parse` / `dhcp_parse_reply`；ASan+UBSan 宿主侧跑（60k 轮缺省、FUZZ\_ITERS 可调），
  发现并修复 **BUG-029**（`icmp_parse` len<14 时 `len-14` 下溢 + `frame+14` 越界读），
  已集成 `run_host_tests.sh` 强制回归

* ✅ **内核堆审计**（v0.29，`heap_audit` 挂入 `kern_audit`）：遍历 `block_t` 链表校验
  magic/free 一致性、size 上界，防 next 指针成环/悬垂；`used/free` 记账计数器与遍历
  统计对账（泄漏/双重释放/写越界破坏块头都会漂移）；报告碎片；宿主单测 + QEMU selftest
  双重锁定（`[audit] heap ok`）

* **record/replay 基础设施**：QEMU `-icount` + gdb reverse-debugging（勿引 rr）；
  日志钉点基于 v0.21 tick 时间戳（已备），失败 transcript 固化回归

* ✅ **回归盲区补格**（v0.29）：

  * `deep`/已生长栈 × fork/exec 组合：新增 `deepfork` / `deepexec` 演示并挂入
    qemu + serial 回归；**顺带抓到并修复 BUG-030**（fork 子进程栈在父槽、守卫按子 pid
    推导槽位误判缺页——改由实际栈位置 `stack_bottom` 推导）

  * brk 收缩-再涨路径：heapdemo step 4（收缩回 8KB→sbrk 再涨复用已映射页）已覆盖

  * 编译产物 × 持久化：test\_persist.sh S10（writefile→ccrun→save→重启→run）已覆盖

### 阶段三「沉淀」（不再是版本号）

> 从"持续开发的仓库"变为"可交付的教学产品"——项目的最终价值不在代码行数，
> 而在"能被多少人学会"。

1. **教学文档系列**：每子系统一篇"从零到一"（引导与保护模式 → 中断与系统调用 →
   分页与隔离 → 调度 → 文件系统 → 网络 → 自举），附最小可运行代码片段 + 思考题
2. **交互式实验手册**：利用已有写-编-跑闭环，读者在 guest 内用 `writefile` + `ccrun`
   编写小程序，亲手体验 `fork`/`brk`/`socket`/`ccboot`——比"读代码"有效得多
3. **开源发布**：定位 **"AI 辅助系统编程的完整案例研究"**——BUG 库的根因/修复/回归
   记录本身就是极有价值的工程方法论素材；演示录屏（`make run` → `selftest PASS`、
   `ccboot` 自举仪式）

### 网络抽象层与虚拟 TCP（netif + 间接 TCP）——新主线

> 版本 v1.1（2026-09-01），状态：**四步已对齐落地**。做真 netif 抽象层，把协议层与具体
> 网卡解耦；串口抽象成第二个网卡；用薄包装虚拟 TCP 应对 HTTP demo，架构预留薄→厚演进
> （演进上限 = guest 侧厚包装伪状态机，本路线图及后续演进均不实现完整 TCP 栈）。
> v1.2（2026-09-02）已迈出"薄→厚"第一级台阶：下行改可靠停-等（stop-and-wait），写入下述
> Step 4 收尾 2；并把后续候选方向「上行可靠」「滑动窗口」显式列入本路线图（见"薄→厚演进候选"）。

**背景**：`netsock.c` 协议层直接调用 `e1000_tx/rx/mac/gw_mac`，且 `udp_build_frame`
在协议层自己拼装完整以太网帧（eth + IP + UDP 三层头），抽象点在驱动层而非协议层——
换网卡须改协议层代码，不可接受。

> **现状清单（收尾口径，Step 3 落地后补充）**：socket 路径（netsock）已全面走 netif，
> `grep e1000_ src/net/` 为空由 CI 守卫强制。剩余"协议在驱动里"的残余仅一处：**e1000 的
> DHCP 租期续约/重获取 BOOTP 组帧**（`e1000.c` 的 `dhcp_send_renew/rebind/reacquire` + timer
> 心跳 `e1000_dhcp_tick` 直调，组帧依赖 `dhcp.c` 的 `dhcp_build_*`）。该处是 e1000 链路层
> 服务（BOOTP 需广播/网关 MAC + e1000\_tx），非通用协议层，**推迟到 HAL 阶段收口**，不属
> Step 3 范围（Step 3 的 grep 只按"符号调用"口径，不受影响）。

**架构决策（已对齐）**

| #  | 决策点         | 结论                                                                                       |
| -- | ----------- | ---------------------------------------------------------------------------------------- |
| D1 | 抽象层包单位      | **IP 数据报层**（lwIP netif 模式）。不做以太网帧层——串口无以太网语义，SLIP 就是 IP over 串口，做帧层会欠"假 MAC + 假 ARP"的技术债 |
| D2 | netif 接口形态  | **ops 表**（init/ready/tx/rx/mac），注册表模式，协议层只依赖接口 = 未来 HAL 设备表原型                            |
| D3 | 串口网卡端口      | **COM2**（QEMU 第二个 `-serial`）。COM1 已被 shell 终端占用，严禁复用                                     |
| D4 | 虚拟 TCP 起步档位 | **薄包装**（HTTP 请求-响应够用）；架构按薄→厚预留，厚包装列为可选项，跑通 demo 后再评估                                     |
| D5 | TCP 语义执行位置  | **宿主转发器**（真 TCP 状态机只在宿主）；guest 侧不实现完整 TCP 栈，演进上限 = 厚包装伪状态机                               |
| D6 | 网卡选择策略      | **静态绑定**（编译开关/启动参数）。不实现路由表；netif 只留"选择策略"接口，将来按目的 IP 选路可平滑升级                             |

**四步落地路径**（每步可独立验证；**每步必须 CI 全绿才能进下一步**）

* [x] **Step 1：netif.h/c + e1000 薄封装**（纯重构，协议语义零变化）：新增 `src/net/netif.h/c`
  ops 表 + 注册表（当前单网卡，按序取第一个）；`src/drv/e1000_netif.c` 把现有驱动函数包进
  ops 表（**驱动本身不改逻辑**）；`netsock.c` 从直调 `e1000_tx/rx` 改为调 netif 接口，
  `udp_build_frame` 的 eth 头拼装下沉到 e1000 适配层。✅ 已完成（PR #11）——验收：7 层 CI 全绿 +
  UDP demo 行为与改造前一致 + diff 纯重构

* [x] **Step 2：COM2 串口适配器（SLIP）**：QEMU 脚本加第二个 `-serial`（COM2）；新增
  `src/drv/uart_netif.c` 实现 SLIP 封装/解封装（RFC 1055：END 0xC0 / ESC 0xDB / ESC\_END
  0xDC / ESC\_ESC 0xDD），注册为第二个网卡；现有 UDP 收发包经 COM2 串口链路打通；CI 新增
  串口网卡单测。✅ 已完成（PR #12，D6 用 `UART_NETIF_DEFAULT` 编译开关静态绑定，PR #13 加固
  对端确定性 + SLIP 并入 fuzz）——验收：guest UDP 数据报经 COM2 到宿主串口对端收发往返一致、
  SLIP 帧边界/转义（含数据中 0xC0/0xDB 用例）通过

* [x] **Step 3：协议层彻底走 netif（eth 封装下沉收尾）**：IP 数据报为 netif 统一包单位，协议层
  （src/net）不再出现任何网卡符号调用；网卡选择静态绑定（默认 e1000，串口按编译开关
  `UART_NETIF_DEFAULT`，D6 已在 Step 2 落地）。✅ 已完成（PR #14）——**边界口径**：
  `grep e1000_ src/net/` 为空且由 CI 守卫强制（运行于宿主层）；`udp_build_frame`/`udp_parse`
  **保留**为共享 etherframe 参考——其消费方是 e1000 链路路径：`dhcp.c` 的 BOOTP 组帧
  （over e1000）与 `e1000.c` UDP selftest，均属"协议在驱动里"的残余，推迟到 HAL 阶段收口
  （见上"现状清单"），故不算协议层 eth 封装。验收：CI 守卫通过 + 8 层 CI 全绿

* [x] **Step 4：宿主转发器 + 虚拟 TCP demo**：会话协议头（`session_id` + `msg_type` +
  `version`（关键预留点） + `flags` + 载荷，消息类型与数据分离、版本字段先行）；宿主
  Python 转发器（按 session\_id 维护 UDP 会话↔TCP 映射：会话表 + 超时清理 + 半开处理 +
  背压；UDP 直转；事件回传 msg\_type=event）；guest 虚拟 TCP 薄包装 API
  `tcp_open/send/recv/close`（fd = **连接对象**结构，非裸整数，厚包装时同一对象加状态机
  字段即可）。✅ 已完成（PR #15）：三份语义规定定稿进 `docs/`（tcp-session-proto / tcp-thin-api /
  tcp-mtu-fail）；薄包装为用户态库（`src/app/tcp.c`）+ `httpdemo` 演示；转发器
  `tests/tcp_proxy.py` 支持 UDP(e1000)+SLIP(COM2) 双通道；fuzz 加会话头 case 7。wire 增补：
  定稿头表原只有宿主事件方向，补 guest→host 控制类 `MSG_OPEN`/`MSG_CLOSE`（tcp_open 的目标
  寻址与 tcp_close 的注销必须承载，见 tcp-session-proto §2.1 v1.1）。验收：HTTP 请求-响应
  demo 跑通（200 OK）+ 断连返回 0 / 拒绝返回 -1 与超时均可区分无挂死 + 事件回传可用 + 双通道全绿。
  收尾（PR #16，BUG-044/045/046）：容量缺陷归并（NET_RXMAX 512→2048、环 1024→4096、转发器下行
  分块 1392、发送硬墙对齐 1400）修"大响应丢尾"；转发器主循环改非阻塞 select 修串口通道饿死；
  test_tcp 自检改重试循环修 CI 稳定误报。httpdemo 增 >1KB/尾字节==TAIL 完整性断言。

* [x] **Step 4 收尾 2（v1.2）可靠下行（stop-and-wait）**：根治"大响应丢尾"的**根因**——
  前序（PR #16）只把 `TCP_RXB` 提到 4096，等于把"丢尾阈值"抬高，并未消掉 NIC/socket 界面
  burst 下**随机丢包**的根因（>16KB 依旧缺尾）。本次把 host→guest 下行改为**可靠停-等**：
  `MSG_DATA` **复用会话头 flags 低 16 位**携带递增序列号 seq（协议留出的厚包装关键预留点，
  v1.1 §2.1 表格与 §5「为厚包装加序列号/ACK 占位」首次启用）；转发器每会话恒 **≤1 报在途**、
  收到 guest 的**累计 ACK**（新增 `MSG_ACK` 0x08，payload = 下一期望 seq 2BE）才发下一个；
  ACK 丢失按 `RETX_MS=2s` 定时重发，guest 遇重复/乱序 seq **幂等丢弃**该报并重发 ACK——
  端到端自愈（SLIP 慢通道重传间隔须 ≥ 单报回环 ~1s，60ms 会灌爆慢 UART；e1000 快通道正常
  不触发）。✅ 已完成：guest 侧 `src/app/tcp.c`（drain 单报 + `rx_next` 期待序号 + `send_ack`）、
  转发器 `tests/tcp_proxy.py`（Session 的 pending/inflight/seq + `_send_next`/`_retransmit`）、
  wire `src/net/tcp_proto.h`（`MSG_ACK` + `tcp_hdr_set/get_seq`）；**新增真·大文件下载 demo
  `src/app/dldemo`**（不复用固定 `TCP_RXB`，每轮 tcp_recv 取 2KB 边收边累加，总长推到 **128KB
  无总字节上限**，剥 HTTP 头后校验 body 尾 7B `EOFTAIL` 完整）+ 回归 `tests/test_tcp_dl.sh`。
  验收：128KB 大文件缺尾**从根子消除**（不再只靠抬缓冲），双通道越跑越稳。

**虚拟 TCP 薄→厚演进候选（v1.2 写入路线图，均未动码）**：下行可靠停-等是"薄→厚"的**第一级
台阶**——guest 侧已开始参与 seq/ACK（收顺序包、回累计 ACK），但**仍属薄包装加固**：guest
没有发送缓冲、重传定时器、窗口/流量控制，不是完整状态机。红线依旧：guest **不做真 TCP 状态机**，
演进上限 = 厚包装伪状态机。下列两项为下一步候选，按"每步可独立验证、CI 全绿才进下一步"推进，
均不需破坏薄包装 API、转发器会话表与协议头结构（GRANT 预留点，见"语义边界"）。

* **上行可靠（guest→host 可靠传输）**：下行已独立落地，上行目前仍是**尽力而为**
  （guest→host `MSG_DATA`，转发器收到即转发、丢不丢看运气）。把停-等**对称搬到上行**：
  guest 侧为发送方向保留发送缓冲 + 分配递增 seq、转发器对已收到的数据报回 ACK、guest 侧
  加重传定时器——使大文件回传 / guest 内程序向宿主写数据同样可靠。wire 增补一条
  host→guest 的 ACK 控制类（或复用 flags 占位），与下行对称；应用 `tcp_send` 语义不变
  （薄包装"一次 send 一发"仍成立，厚包装流式拼接预留 `txq` 就此启用）。

* **滑动窗口（停-等提速，性能项）**：停-等"发 1 等 ACK"，任何时刻链路上只有 1 报在飞，
  慢通道带宽亏一半以上。允许每会话 in-flight = **窗口 W**（≥1），时序维持累计 ACK、
  仅重传丢失的那一报——吞吐随窗口线性提升。依赖的 seq 机制已具备；窗口上限由 L2/MTU 与
  两端缓冲能力决定，初期取保守小值（如 4），对齐"先跑通再优化"。属可靠性之后的性能优化，
  与上行可靠正交、可独立推进。

**虚拟 TCP 语义边界（诚实声明：这是包装，不是 TCP）**：应用体感取决于包装厚度——

| | 薄包装（当前目标） | 厚包装（演进上限，见下） |
|---|---|---|
| guest API | `tcp_open/send/recv/close` | 同左，签名不变 |
| 底层 | UDP 数据报 + 宿主转发器 | 薄包装 + 本地伪状态机（序列号/超时重传/乱序缓冲/伪 ACK） |
| 应用体感 | "调用像 TCP"，语义仍请求-应答（一次 send 对一次 recv） | 接近真 TCP |
| 丢包/断连 | 超时感知 | 重传兜底，应用基本无感 |
| 适用 | HTTP 请求-响应 demo | 流式 / 长连接场景 |
| 成本 | 低 | 高，测试面显著增大 |

TCP 给应用的三个体感来源，薄包装只给三分之一：**流式**（仍按报文）、**可靠**（重传在宿主做，
guest 无感；v1.2 起下行由转发器重传兜底、guest 仅幂等收敛）、**状态可见**（靠转发器显式回传
连接事件，`msg_type=event` 决定薄包装体验下限）。

**架构预留（薄→厚演进，长期接口契约，现在就必须按此设计）**：将来升级厚包装时**应用代码零改动、
协议帧格式不破坏、转发器新旧会话共存**。对应实现见上文"薄→厚演进候选"（已落地一项：v1.2
下行可靠停-等）。

1. **连接句柄 = 对象**：`tcp_open` 返回的 fd 映射到"连接对象"（session_id / 状态 / 收发缓冲 /
   事件队列），厚包装只是给同一对象加状态机字段，API 签名不变。
2. **收发走流式缓冲**：`tcp_send/recv` 内部一律经"发送队列 / 接收缓冲"，薄包装一次 send=一条
   报文，但数据结构按流式设计（可拼接/分段）；厚包装的乱序缓冲/重传队列直接复用。
3. **控制/数据通道分离**：`msg_type` 区分数据与事件（建/关/败/超时）；薄包装已具备事件感知，
   厚包装的事件驱动天然可用，无需改协议。
4. **协议头版本字段先行**：`version` + `flags` 必须占位。seq/ACK 已落地（v1.2 起 `MSG_DATA`
   flags 低 16 位作下行 seq）+ 控制类 `msg_type` 新增 `MSG_ACK`，**均不改帧格式**；转发器按
   version 分发，旧薄会话与新会话共存。
5. **转发器保持无状态协议、可扩展**：只按 `session_id + version` 工作，不假设"每 session 必为
   请求-应答"；厚会话只是载荷里多了字段，转发路径不变。

**一句话**：薄包装是"API 形状 + 事件通道"先立起来，厚包装是"在既有对象和通道里补状态机"——
架构上不堵路。诚实声明：这是包装，不是 TCP（不实现 guest 内真 TCP 状态机，红线见下）。

### 红线（明确不做）

| 方向                      | 理由                                                                                                                         |
| ----------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| guest 内 TCP 状态机         | 复杂度过高、测试面超纲。不实现 guest 内真 TCP；虚拟 TCP 用薄包装（真 TCP 状态机只在宿主转发器，guest 演进上限 = 厚包装伪状态机），见上"网络抽象层"节。若确需真 TCP 属 virtio-net 另起炉灶的独立项目 |
| 多核 / SMP                | 重写调度/锁/页表模型，等于重写                                                                                                           |
| HAL / ARM 移植            | 无真实目标硬件，抽象层设计必然过度；等真有 ARM 板子                                                                                               |
| 动态链接 / ELF 重定位          | 已标注"黑洞，吃掉项目余生"                                                                                                             |
| 移植 GCC/clang/TinyCC 完整体 | 同上                                                                                                                         |
| 图形子系统                   | 偏离核心教学定位                                                                                                                   |

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

  * **网络进一步可用化**：~~DHCP 租期续约（T1/T2 定时 renew）~~ ✅ **v0.28 已完成**；
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
    \= v0.3 懒分配 + v0.13 守卫页两个已验证机制的组合。~~ ✅ **v0.26#1 已完成**
    （32KB 槽 = 硬底守卫页 + 28KB 可生长栈区，三态判定，`deep` 演示 12KB 递归触发 3 次生长）

  * **`sys_brk`** **用户堆**：~~现无堆（`map_frames[8]`~~ ~~固定、`sys_map_page`~~ ~~一次一页）。在私有页
    之上开可伸缩区，记账进 PCB；记账表 kmalloc 动态化（为编译器 malloc 铺路）。~~ ✅ **v0.26#2 已完成**
    （SYS\_BRK 查询/设置 program break，堆区 320KB，扩展按页补映射、收缩保留映射复用，
    `heapdemo` 演示 + `test_brk` 宿主单测）

  * **ELF 加载去上限**：~~`load_frames[APP_MAXFRAMES=8]`~~ ~~改动态列表、app 区 16KB 扩 MB 级；
    顺带解决~~ ~~`APP_LINK`~~ ~~单槽掩护的结构债（并发跑两个同链接地址程序）。~~ ✅ **v0.26#3 已完成**
    （load\_frames/own\_frames 动态化、app 区 1MB、用户空间 16MB；`bigdemo` 70KB/21 帧验证）

  * 三项均可纯逻辑化宿主单测（stack\_guard\_hit 边界、brk 状态机、加载器 frames 记账），
    延续现有测试风格。

* **v0.27-29「工具链与自举」（迄今最大单版本，必须拆小）**

  * **移植对象修正**：简报原指"Rob Pike c5"——核实后 c5 大概率是 8086 16 位版本，与 i386
    32 位平坦模型不匹配，**不直接采用**。更稳妥候选：

    * **cc500**（E. Grimley-Evans，\~750 行）：stdin 读 C → stdout 出 **x86-32 ELF**，自托管、
      无 libc 依赖（内置 exit/getchar/malloc/putchar 机器码，malloc 用 brk 实现）——
      与我们的 ELF 加载器 + v0.26 `sys_brk` 天然衔接（GPL-2.0，参考/自写）；

    * 或自写 C 子集前端 + 简单 x86-32 代码生成。

    * 可行性锚点（简报成立）：PWB/C 曾以 56KB 内存在 PDP-11 自举；128MB QEMU + 容量三连后
      属"过于富裕"的尺度。

  * **~~Micro-OS libc~~**：~~user\_lib.h 已是雏形——补 printf/malloc/brk + open/read/write/exec
    包装 + crt0；纯逻辑部分（printf 状态机、malloc 堆算法）复用 heap.c 经验宿主单测。~~
    ✅ **v0.27 已部分达成**：cc500 自带极简运行时（syscall3 + malloc/exit/sys\_print），
    用户态已可用 `sys_*` + 共享头 `user_lib.h`；完整 printf/格式化输出仍可作后续增量。

  * **红线（不要走的路）**：不移植 GCC/clang/TinyCC 完整体、不做动态链接（黑洞，吃掉项目余生）。

  * **版本拆分**：27a 汇编器+链接器跑通（手工输入出可执行 ELF 进 FS、被 `run` 执行）；
    27b C 子集前端 → 全链通；28 libc + crt0 + 若干样例程序；29 自举 + 端到端回归通道。
    ✅ **v0.27 已一步达成 27a/27b/29 核心**：直接移植自托管编译器 cc500，guest 内
    `cc500 编译自身 → P1；P1 再编译 → P2；P1==P2` 自举闭环已跑通（`shell ccboot` 命令，
    `[ccboot] … PASS`）。

  * **v0.29 自举仪式验收**：~~`cc.c`~~ ~~编译出~~ ~~`cc2`，`cc2`~~ ~~再编译~~ ~~`cc.c`，产物逐字节一致；~~ ✅
    已达成（cc500 对自身源码是"不动点"，P1==P2 逐字节一致）。
    ~~剩余增量：让编译器支持**命令行指定输入/输出路径**（现为固定~~ ~~`/cc500.c`~~ ~~→~~ ~~`/out.elf`），
    然后演示剧本：agent 经串口/UDP 通道 →~~ ~~`cat > hello.c`~~ ~~→~~ ~~`cc hello.c`~~ ~~→~~ ~~`run a.out`；~~
    ✅ **v0.27b 已完成**：cc500 支持 `argv[1]=输入 argv[2]=输出`（缺省回退固定路径）；
    shell 新增 `writefile <path> <content>`（agent 写源码）+ `ccrun <src> <out>`
    （编译并运行）；guest 内 `writefile /hello.c … → ccrun /hello.c /hello.elf` →
    编译产物被加载运行全链路跑通（test\_serial.sh 用例）。
    ~~⚠️ v0.29 发现（BUG-032）：argv 路径仅对 gcc 版成立、自编译产物静默丢参；~~
    ✅ **v0.30 已修复**：入口桩编组 argc/argv 后，自编译产物（P1）exec 带 argv 也正确。

* **P2 并行项（可插在网络收尾之前/之间）**

  * **record/replay 提前**（简报论点成立：v0.20 "e1000 MMIO 高地址 × 页目录只克隆低 1GB"
    类跨子系统 bug 证明组合爆炸已开始，每加子系统它越便宜）。
    **修正**：不引 rr 工具（rr 面向 x86-64 原生，不直接适配 QEMU guest），改
    **QEMU** **`-icount`** **+ gdb reverse-debugging**（TCG i386 原生支持），日志钉点基于
    v0.21 tick 时间戳（已备），失败 transcript 固化回归。

  * **~~DHCP 租期续约（T1/T2 renew）~~**：✅ **v0.28 已完成**（RFC 2131 §4.4.5：
    T1 单播 RENEW / T2 广播 REBIND / ACK 重置 / NAK·超时重新获取；netsock 端口 68
    专用 socket 解决用户 recvfrom"排空"网卡抢先消费应答；test\_net 短租期回归闭环）。
    TCP 状态机仍属"非最小可演进"核心，暂缓。

## 开发原则

1. **每步可运行**：任何一次改动后 `make test` 必须全绿（宿主单测 + QEMU 回归）。
2. **纯逻辑可单测**：与硬件解耦的策略（堆、键盘映射、调度队列）抽成无内核依赖模块，用宿主单测覆盖。
3. **先跑通再优化**：优先正确的功能，再谈性能与安全。
4. **文档随代码走**：每个版本同步更新 changelog / bugs / design。

