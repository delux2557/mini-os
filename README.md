# Mini-OS

一个从零开始、可运行在 QEMU 虚拟机上的微型 x86 操作系统。
用于学习操作系统核心原理：引导、中断、内存、用户态、进程调度、文件系统、
网络，以及**自举（guest 内编译并运行程序）**。

> 项目已达成"教学闭环"（进程/内存/文件/网络/工具链全部完成），
> 进入 **"收尾-加固-沉淀"** 阶段——最终交付物 = 可运行内核 + 工程方法论文档
> + "AI 辅助系统编程"的实证案例（演进路线见 [docs/explanation/roadmap.md](docs/explanation/roadmap.md)）。
>
> 代码由 AI 辅助编写；每个版本先跑通最小可用目标，再迭代扩展。
> **文档导航见 [docs/README.md](docs/README.md)**（清单 + 生命周期 + 维护规则）：
> 设计与思路见 [docs/explanation/design.md](docs/explanation/design.md)，Bug 记录见 [docs/reference/bugs.md](docs/reference/bugs.md)，
> 版本变更见 [docs/reference/changelog.md](docs/reference/changelog.md)，演进路线见 [docs/explanation/roadmap.md](docs/explanation/roadmap.md)。

## 版本矩阵（精简）

> 完整版本史（v0.1 起，含 netif/RR 各版本）见 [docs/reference/changelog.md](docs/reference/changelog.md)——**版本历史的唯一事实源**。
> v0.1~v0.33 里程碑速览另见 [docs/history/roadmap-milestones.md](docs/history/roadmap-milestones.md)（只读归档）。

| 版本   | 里程碑 |
| ---- | ------ |
| v0.31 | 内核资源归属收口：per-process fd 表 + socket 归属/退出回收/保留槽防任意 close |
| v0.32 | cc500 编译器三缺陷修复：未闭合字符串自噬 / 未定义符号静默 / 关系运算残缺 + error 诊断 |
| v0.33 | 回归可观测性收口：selftest 行撕裂 / pid 表静默 + harness 退出码统一 + CI 全链 |

> 当前主线已到 **v1.x**（网络抽象层 netif + 虚拟 TCP 薄包装 → 上行滑动窗口，见 changelog）。

### cc500 方言边界（guest 内写-编-跑须知）

- **数组声明不支持（局部与全局均不支持）**：cc500 的声明文法只有 `type id [=expr] ;`，无 `[` 分支，
  `int arr[4];`、`char s[8];` 在文法层面一律被拒（C 子集边界，非缺陷）。对**已有缓冲**（如 `malloc` 取来
  的字节区、`token`/字符串）的**下标访问 `p[i]` 是支持的**——批量/集合数据用独立变量 + 手动缓冲 + 下标模拟；
  详见 `v2-c-kernel/tools/cc500/` 方言实现边界
- **编译错误诊断**：v0.32 起 `error()` 打印 `cc500: error at <token>`（此前裸 `exit(1)` 零诊断，
  排错靠二分试错）；未闭合字符串→`bad string`、未定义符号→`undefined symbol` 均有专项消息
- **argv 路径已通**：入口桩自 v0.30 起编组 argc/argv，gcc 版与自编译产物（P1）exec
  带 argv 均正确（历史 BUG-032 已修复）

## 目录结构

```
mini-os/
├── README.md              # 本文件（快速上手 + 文档导航）
├── .gitignore
├── docs/                  # 文档（总入口见 docs/README.md；Diátaxis 分层）
│   ├── README.md          # ★ docs 总入口：文档清单 + 生命周期 + 维护规则
│   ├── explanation/       # 理解导向（活文档）
│   │   ├── design.md      # 架构设计与开发思路
│   │   └── roadmap.md     # 演进路线（只留未完成主线；已完成段归档 history/）
│   ├── reference/         # 信息导向（活文档）
│   │   ├── changelog.md   # 版本变更日志（版本历史唯一事实源）
│   │   └── bugs.md        # Bug 记录（编号被代码引用，严禁改动）
│   ├── tcp-session-proto.md  # 虚拟 TCP 会话协议头规范（契约）
│   ├── tcp-thin-api.md       # 虚拟 TCP 薄包装 API 契约表（契约）
│   ├── tcp-mtu-fail.md       # MTU/大包失败路径规范（契约）
│   ├── history/           # 已归档时点产物（只读）：netif 路线图 / 里程碑表 / external-reviews 评审账本
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

## 许可证

本项目采用**混合授权**：

* **内核代码（`v2-c-kernel/src/` 等）**：采用 [MIT 许可证](LICENSE)。
* **`v2-c-kernel/tools/cc500/` 下的 cc500 编译器**：派生自 Edmund GRIMLEY EVANS 的
  cc500（Copyright © 2006，orig: http://homepage.ntlworld.com/edmund.grimley-evans/cc500/），
  按 **GPL-2.0-or-later** 提供；许可证全文见
  [`tools/cc500/LICENSE`](v2-c-kernel/tools/cc500/LICENSE)，来源与边界说明见
  [`tools/cc500/README.md`](v2-c-kernel/tools/cc500/README.md)。

## 快速开始

依赖：`gcc`(m32 支持)、`nasm`、`ld`、`objcopy`、`qemu-system-i386`、`python3`、`socat`。
`qemu-user`（提供 `qemu-i386`）为**测试可选**：宿主内核无 ia32 exec 时，`make test-cc500` 的宿主层 hostcc 需用其运行 32 位二进制。

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

