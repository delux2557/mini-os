# v2-c-kernel（当前版本）

x86 32 位 C 内核，multiboot 引导，运行于 QEMU。当前里程碑：v0.33 回归可观测性收口（F-4 selftest 行撕裂 / F-5 pid 表静默 + harness 退出码统一 + CI 全链）。

```
src/     内核源代码（.c/.h/.s/.ld）
tests/   宿主单元测试 + QEMU 回归脚本（五层）
build/   构建产物（gitignore，make clean 清理）
```

## 常用命令

```bash
make            # 构建 -> build/kernel.elf
make run        # 图形界面运行
make run-serial # 无图形界面运行，串口日志 -> build/serial.log
make test-host  # 宿主单元测试（纯逻辑，秒级）
make test-qemu  # QEMU 回归（键盘 sendkey 路径）
make test-serial # QEMU 串口终端回归（模拟外部 agent 经串口驱动 shell）
make test-persist # QEMU ATA 真盘持久化回归（两次运行共享磁盘镜像）
make test-net   # QEMU 网络回归（e1000 TX/RX + ARP + UDP + ICMP 与宿主端到端）
make test       # 全部测试（五层）
make clean
```

## 与操作系统交互

```bash
# 方式一：图形界面 + 键盘（本地桌面）
make run

# 方式二：串口终端（无界面模式，可被脚本/另一个 AI agent 驱动）
qemu-system-i386 -kernel build/kernel.elf -display none -serial stdio -monitor none
#   外部进程向 QEMU stdin 写入即输入，读 stdout 即输出（双向终端）
#   shell 命令：help / ls / cat / mkdir / rmdir / rm / run / exec / save / selftest / exit

# 方式三：ATA 真盘持久化（用户数据跨重启存活）
qemu-system-i386 -kernel build/kernel.elf -hda disk.img -display none -serial stdio -monitor none
#   首次启动格式化空白盘；mkdir/cat 后执行 `save` 写回磁盘，重启后再挂同一镜像数据仍在
```

## 单行结构化自检（agent 可验证）

shell 内置 `selftest` 命令：逐跑 hello/isol/forkdemo/fsdemo/waitdemo 五个代表应用
（覆盖 spawn / 隔离 / fork / 文件系统 / wait 语义），再追加第 6 项——内核自审计
（帧配平 / 信号量守恒 / PCB 状态机），汇总输出一行：

```
[selftest] PASS (6 checks)
```

外部 agent 只需 grep 这一行即可完成全量确认，无需逐条匹配几十个里程碑。

## syscall 边界校验（v0.17）

所有涉用户指针的系统调用（print / readline / spawn / exec / wait / 文件系统全链路）
都会先校验指针是否落在用户空间（`userptr.c` 的 `copyin/copyout/copyin_str`），
用户程序无法借 syscall 读写内核内存。`run abuse` 演示：用内核低地址/回绕地址
调用各类 syscall 全部被拒（-1），合法路径不受影响，输出 `[abuse] verify OK`。

## 依赖

gcc(-m32) / nasm / ld / objcopy / qemu-system-i386 / python3 / socat

`qemu-user`（`qemu-i386`）测试可选：宿主无 ia32 exec 时 `make test-cc500` 的 hostcc 用它运行 32 位二进制。

## 文档

架构与开发思路、演进路线、Bug 记录、版本日志见项目根目录 [docs/](../docs/)。
