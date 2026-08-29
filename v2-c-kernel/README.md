# v2-c-kernel（当前版本）

x86 32 位 C 内核，multiboot 引导，运行于 QEMU。当前里程碑：v0.16 用户态 CRT 收口 + ATA 真盘持久化 + 单行结构化自检。

```
src/     内核源代码（.c/.h/.s/.ld）
tests/   宿主单元测试 + QEMU 回归脚本（四层）
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
make test       # 全部测试（四层）
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
（覆盖 spawn / 隔离 / fork / 文件系统 / wait 语义），汇总输出一行：

```
[selftest] PASS (5 checks)
```

外部 agent 只需 grep 这一行即可完成全量确认，无需逐条匹配几十个里程碑。

## 依赖

gcc(-m32) / nasm / ld / objcopy / qemu-system-i386

## 文档

架构与开发思路、演进路线、Bug 记录、版本日志见项目根目录 [docs/](../docs/)。
