# Micro-OS

一个从零开始、可运行在 QEMU 虚拟机上的微型 x86 操作系统。
用于学习操作系统核心原理：引导、中断、内存、用户态、进程调度。

> 代码由 AI 辅助编写；每个版本先跑通最小可用目标，再迭代扩展。
> 历史与路线见 [docs/roadmap.md](docs/roadmap.md)，设计与思路见 [docs/design.md](docs/design.md)，
> Bug 记录见 [docs/bugs.md](docs/bugs.md)，版本变更见 [docs/changelog.md](docs/changelog.md)。

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

依赖：`gcc`(m32 支持)、`nasm`、`ld`、`objcopy`、`qemu-system-i386`。

```bash
cd v2-c-kernel

make            # 构建内核 -> build/kernel.elf
make run        # 带图形界面运行（QEMU）
make run-serial # 无图形界面运行，串口日志写到 build/serial.log

make test-host  # 宿主单元测试（纯逻辑，秒级）
make test-qemu  # QEMU 自动回归（串口日志关键字校验）
make test       # 以上全部
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

