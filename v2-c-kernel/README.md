# v2-c-kernel（当前版本）

x86 32 位 C 内核，multiboot 引导，运行于 QEMU。当前里程碑：v0.10 串口终端（外部 agent 可经 QEMU 交互）。

```
src/     内核源代码（.c/.h/.s/.ld）
tests/   宿主单元测试 + QEMU 回归脚本
build/   构建产物（gitignore，make clean 清理）
```

## 常用命令

```bash
make            # 构建 -> build/kernel.elf
make run        # 图形界面运行
make run-serial # 无图形界面运行，串口日志 -> build/serial.log
make test-host  # 宿主单元测试
make test-qemu  # QEMU 自动回归（键盘 sendkey 路径）
make test-serial # QEMU 串口终端回归（模拟外部 agent 经串口驱动 shell）
make test       # 全部测试
make clean
```

## 与操作系统交互

```bash
# 方式一：图形界面 + 键盘（本地桌面）
make run

# 方式二：串口终端（无头、可被脚本/另一个 AI agent 驱动）
qemu-system-i386 -kernel build/kernel.elf -display none -serial stdio -monitor none
#   外部进程向 QEMU stdin 写入即输入，读 stdout 即输出（双向终端）
#   shell 命令：help / ls / cat motd / run hello|echo|crash / exit
```

## 依赖

gcc(-m32) / nasm / ld / objcopy / qemu-system-i386

## 文档

架构与开发思路、演进路线、Bug 记录、版本日志见项目根目录 [docs/](../docs/)。
