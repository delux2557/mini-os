# Mini-OS

一个从零开始、可运行在 QEMU 虚拟机上的微型 x86 操作系统。
用于学习操作系统核心原理：引导、中断、内存、用户态、进程调度、文件系统、
网络，以及**自举（guest 内编译并运行程序）**。

> 项目已达成"教学闭环"（进程/内存/文件/网络/工具链全部完成），
> 进入 **"收尾-加固-沉淀"** 阶段——最终交付物 = 可运行内核 + 工程方法论文档
> + "AI 辅助系统编程"的实证案例（演进路线见 [docs/roadmap.md](docs/roadmap.md)）。
>
> 代码由 AI 辅助编写；每个版本先跑通最小可用目标，再迭代扩展。
> 设计与思路见 [docs/design.md](docs/design.md)，Bug 记录见 [docs/bugs.md](docs/bugs.md)，
> 版本变更见 [docs/changelog.md](docs/changelog.md)。

## 版本矩阵

| 版本   | 目录             | 里程碑                     | 截图                                           |
| ---- | -------------- | ----------------------- | -------------------------------------------- |
| v0.1 | `v1-floppy/`   | 软盘引导，进入保护模式，VGA 输出      | [v1](docs/screenshots/v1_protected_mode.png) |
| v0.2 | `v2-c-kernel/` | C 内核 + IDT/PIC/定时器/键盘   | [v0.2](docs/screenshots/v2_v02.png)          |
| v0.3 | `v2-c-kernel/` | 分页 + 内核堆 + 懒分配          | [v0.3](docs/screenshots/v2_v03.png)          |
| v0.4 | `v2-c-kernel/` | 用户态 ring3 + 系统调用 + 内存保护 | [v0.4](docs/screenshots/v2_v04.png)          |
| v0.5 | `v2-c-kernel/` | 抢占式多任务 + 轮转调度 + 进程管理    | —                                            |
| v0.6 | `v2-c-kernel/` | IPC：信号量 + 互斥锁 + 共享内存 + rendezvous | — |
| v0.7 | `v2-c-kernel/` | IPC：有界消息队列 + 生产者-消费者（阻塞/唤醒） | — |
| v0.8 | `v2-c-kernel/` | 文件系统：内存盘 + 极简 mini-fs（create/open/read/write/ls/delete） | [v0.8](docs/screenshots/v2_v08.png) |
| v0.9 | `v2-c-kernel/` | ELF 加载器 + 常驻交互式 Shell（help/ls/cat/run）+ initramfs | [v0.9](docs/screenshots/v2_v09.png) |
| v0.10 | `v2-c-kernel/` | 串口终端：外部 agent 经 QEMU 串口驱动 shell（`-serial stdio`） | — |
| v0.11 | `v2-c-kernel/` | 每进程独立地址空间 + 物理内存隔离（isol 演示） | — |
| v0.12 | `v2-c-kernel/` | fork/exec 进程模型 + argv 参数传递 | — |
| v0.13 | `v2-c-kernel/` | 用户栈守卫页 + 栈溢出检测 | — |
| v0.14 | `v2-c-kernel/` | FS 增强：目录层级 / 间接块(4.1MB) / seek / 追加写 | — |
| v0.15 | `v2-c-kernel/` | 补全 wait/waitpid 语义 + 孤儿清理 | — |
| v0.16 | `v2-c-kernel/` | 用户态 CRT 收口 + ATA 真盘持久化 + 单行 selftest | — |
| v0.17 | `v2-c-kernel/` | syscall 边界校验（copyin/copyout）+ abuse 演示 | — |
| v0.18 | `v2-c-kernel/` | 网络：e1000 网卡驱动 + 极简 ARP（PCI + SLIRP 端到端） | — |
| v0.19 | `v2-c-kernel/` | 网络加厚：极简 IP/UDP（纯逻辑可宿主单测） | — |
| v0.20 | `v2-c-kernel/` | 网络可用：用户态 UDP socket（sockdemo 端到端回环） | — |
| v0.21 | `v2-c-kernel/` | 内核自审计（不变量检查）+ syscall 边界契约化 | — |
| v0.22 | `v2-c-kernel/` | 网络交互化：shell `netping` 命令 | — |
| v0.23 | `v2-c-kernel/` | ICMP Echo：PING 通宿主 | — |
| v0.24 | `v2-c-kernel/` | UDP 校验和错误路径（坏包拒绝） | — |
| v0.25 | `v2-c-kernel/` | DHCP 客户端：动态获取 IP/网关（失败回退静态） | — |
| v0.26 | `v2-c-kernel/` | 容量三连：用户栈按需生长 + 用户堆(brk) + ELF 加载去上限 | — |
| v0.27 | `v2-c-kernel/` | 工具链与自举：cc500 编译器移植 + guest 内写-编-跑闭环（ccboot 自举不动点） | — |
| v0.27b | `v2-c-kernel/` | cc500 命令行路径 + shell `writefile`/`ccrun`（任意程序写-编-跑） | — |
| v0.28 | `v2-c-kernel/` | DHCP 租期续约（T1 单播 RENEW / T2 广播 REBIND，RFC 2131） | — |
| v0.29 | `v2-c-kernel/` | 加固：宿主侧 fuzz + 内核堆审计 | — |
| v0.30 | `v2-c-kernel/` | 修复工具链严重 BUG（文件槽泄漏 + 自编译产物丢 argv）+ 代码审查修复 | — |
| v0.31 | `v2-c-kernel/` | 内核资源归属收口：per-process fd 表（fd 号进程私有）+ socket 归属/退出回收/保留槽防任意 close | — |
| v0.32 | `v2-c-kernel/` | cc500 编译器三缺陷修复：未闭合字符串自噬 / 未定义符号静默 / 关系运算残缺 + error 诊断 | — |
| v0.33 | `v2-c-kernel/` | 回归可观测性收口：F-4 selftest 行撕裂 / F-5 pid 表静默 + harness 退出码统一 + CI 全链 | — |

### cc500 方言边界（guest 内写-编-跑须知）

- **全局数组不支持**：`int arr[4];` 在文法层面被拒（C 子集边界，非缺陷），用局部数组
  或手动缓冲替代
- **编译错误诊断**：v0.32 起 `error()` 打印 `cc500: error at <token>`（此前裸 `exit(1)` 零诊断，
  排错靠二分试错）；未闭合字符串→`bad string`、未定义符号→`undefined symbol` 均有专项消息
- **argv 路径已通**：入口桩自 v0.30 起编组 argc/argv，gcc 版与自编译产物（P1）exec
  带 argv 均正确（历史 BUG-032 已修复）

## 目录结构

```
mini-os/
├── README.md              # 本文件
├── .gitignore
├── docs/                  # 文档与截图
│   ├── roadmap.md         # 演进路线
│   ├── design.md          # 架构设计与开发思路
│   ├── bugs.md            # Bug 记录
│   ├── changelog.md       # 版本变更日志
│   └── screenshots/       # 各版本运行截图
├── v1-floppy/             # 历史版本：软盘引导（冻结）
│   ├── boot.asm
│   ├── boot.bin
│   └── os.img
└── v2-c-kernel/           # 当前开发版本：C 内核
    ├── Makefile
    ├── src/               # 内核源代码（.c/.h/.s/.ld）
    ├── tests/             # 宿主单测 + QEMU 回归脚本
    └── build/             # 构建产物（gitignore，make clean 清理）
```

## 快速开始

依赖：`gcc`(m32 支持)、`nasm`、`ld`、`objcopy`、`qemu-system-i386`、`python3`、`socat`。

```bash
cd v2-c-kernel

make            # 构建内核 -> build/kernel.elf
make run        # 带图形界面运行（QEMU）
make run-serial # 无图形界面运行，串口日志写到 build/serial.log

make test-host  # 宿主单元测试（纯逻辑，秒级）
make test-qemu  # QEMU 自动回归（串口日志关键字校验）
make test-serial # QEMU 串口终端回归（模拟外部 agent 经串口驱动 shell）
make test-persist # QEMU ATA 真盘持久化回归（两次运行共享磁盘镜像）
make test-net   # QEMU 网络回归（e1000 + ARP + UDP + ICMP 与宿主端到端）
make test       # 以上全部（五层）
make clean      # 清理 build/
```

## 技术栈

* 架构：x86 32 位保护模式（multiboot 引导，QEMU 加载）

* 语言：C（内核）+ 汇编（boot/中断/上下文切换）+ NASM

* 核心子系统：GDT/IDT、8259 PIC、PIT 定时器、PS/2 键盘、VGA/串口、
  物理页帧分配、分页与懒分配、内核堆、ring3 用户态、系统调用、进程调度、
  信号量/互斥锁/共享内存/消息队列（IPC 同步与通信）、内存盘文件系统、
  ELF32 可执行程序加载、交互式 Shell

## 测试与可靠性

* **宿主单元测试**：把无内核依赖的纯逻辑（堆、键盘行缓冲、调度队列、信号量、消息队列、文件系统、ELF 加载器）编译成普通 Linux 程序断言，秒级反馈

* **QEMU 回归**：无图形界面运行内核 → 抓串口日志 → 校验关键里程碑（进程创建/抢占/唤醒/回收/idle 心跳/信号量同步/消息队列收发/文件读写/ls 列目录）；v0.9 起经 HMP monitor `sendkey` 交互式注入 shell 命令端到端验证（`help/ls/cat motd/run hello/run echo/run crash`）

