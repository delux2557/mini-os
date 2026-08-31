#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/qemu_regression.sh
# QEMU 自动回归测试：
#   - 无图形界面运行内核 -> 抓串口日志 -> 校验关键里程碑（v0.1 ~ v0.8）
#   - v0.9：通过 HMP monitor(unix socket) sendkey 注入按键，交互式驱动 shell
#     （help / ls / cat motd / run hello / run echo / run crash）
# 失败项会列出缺失的关键字，便于定位内核某一步没有走到。
set -u
cd "$(dirname "$0")/.." || exit 1
# v0.33 harness 约定：exit 0=全绿 / 1=断言失败 / 2=环境或依赖缺失（构建失败仍为 [FAIL]/exit 1，属代码真红）
for c in qemu-system-i386 socat nasm gcc; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

LOG="build/serial.log"
MON="/tmp/minios-mon.sock"
DURATION="${DURATION:-35}"

echo "== [1/4] 构建内核 =="
if ! make >/dev/null 2>&1; then
    echo "[FAIL] 内核构建失败"
    exit 1
fi
echo "      构建完成"

echo "== [2/4] QEMU 无图形界面运行 ${DURATION}s（HMP monitor 供 sendkey） =="
rm -f "$LOG" "$MON"
qemu-system-i386 -kernel build/kernel.elf -display none -serial file:"$LOG" \
    -no-reboot -no-shutdown -m 64 \
    -monitor unix:"$MON",server,nowait >/dev/null 2>&1 &
QPID=$!
QSTART=$SECONDS

# 等待 shell 提示符出现（即 shell 已阻塞在 readline 上，可安全注入命令）
for _ in $(seq 1 40); do
    grep -q 'mini-os\$ ' "$LOG" 2>/dev/null && break
    sleep 0.25
done

# ---- 向 HMP monitor 发一组 sendkey（socat 连接），逐字符转成 sendkey ----
sendkeys() {   # sendkeys <字符串>；空格=spc，换行=ret
    local s="$1" ch cmds=""
    local i
    for ((i = 0; i < ${#s}; i++)); do
        ch="${s:$i:1}"
        case "$ch" in
            ' ')   cmds+="sendkey spc\n" ;;
            $'\n') cmds+="sendkey ret\n" ;;
            *)     cmds+="sendkey $ch\n" ;;
        esac
    done
    printf "%b" "$cmds" | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1
}

# ---- 等待日志"自起始行之后新增内容"中出现关键字 ----
INTERACTIVE_FAIL=0
wait_after() {   # wait_after <起始行> <说明> <正则> [超时秒]；命中返回 0，超时返回 1（不计数）
    local start="$1" desc="$2" re="$3" tmo="${4:-8}"
    local i
    for ((i = 0; i < tmo * 4; i++)); do
        if tail -n +$((start + 1)) "$LOG" 2>/dev/null | grep -q "$re"; then
            echo "[ok]   $desc"
            return 0
        fi
        sleep 0.25
    done
    echo "[FAIL] 未等到 $desc (匹配: $re)"
    return 1
}

# ---- 注入一条命令并等待其若干输出标记（基线=发送前行号，避免命中旧输出/漏掉同步写入）。
# 偶发 socat/sendkey 注入丢失时自动重发一次；仅最终失败才计入 INTERACTIVE_FAIL。 ----
cmd() {   # cmd <说明前缀> <命令串> [等待正则...]
    local desc="$1" s="$2"; shift 2
    local start re attempt ok
    for attempt in 1 2; do
        start=$(wc -l < "$LOG")
        sendkeys "$s"
        ok=1
        for re in "$@"; do
            wait_after "$start" "$desc" "$re" || ok=0
        done
        [ "$ok" -eq 1 ] && return 0
        sleep 0.5   # 注入重试前略等（让 shell 回到提示符）
    done
    INTERACTIVE_FAIL=$((INTERACTIVE_FAIL + 1))
}

echo "== [3/4] 交互式注入 shell 命令 =="
cmd "shell help"     "help
"      "mini-os shell commands:"
cmd "shell ls"       "ls
"      "\[ls\] /:"
cmd "cat motd"       "cat motd
"      "Mini-OS v0.32: toolchain self-host (cc500 compiles itself)"
cmd "run hello"      "run hello
"      "Hello from 'hello' app! pid=" "\[shell\] 'hello' exited code=0"
cmd "run echo"       "run echo
"      "\[echo\] type a line and press Enter"
cmd "echo 输入"      "hi
"      "\[echo\] got 2 bytes: \[hi\]"
cmd "run crash"      "run crash
"      "\[crash\] writing kernel memory" "\[shell\] 'crash' exited code="
cmd "run isol"       "run isol
"      "\[isol\] pid=.* map ok addr=0x80050000" "\[isol\] pid=.* ISOLATED OK" "\[shell\] 'isol' exited code=0"
# ---- v0.12 fork / exec ----
cmd "run forkdemo"   "run forkdemo
"      "\[fork\] pid=.* before fork" "\[fork\] PARENT pid=.* fork returned child=" "\[fork\] CHILD pid=.* fork returned 0" "\[fork\] pid=.* ISOLATED OK" "\[shell\] 'forkdemo' exited code=0"
cmd "exec args"      "exec args alpha beta gamma
"      "\[exec\] pid=.* -> 'args'" "\[args\] pid=.* argc=4" "\[args\] argv\[1\]='alpha'" "\[args\] argv\[3\]='gamma'" "\[shell\] 'args' exited code=0"
# ---- v0.13 栈守卫页 ----
cmd "run stackovf"   "run stackovf
"      "\[stackovf\] pid=.* starting" "\[user\] STACK OVERFLOW pid=" "\[shell\] 'stackovf' exited code="
# ---- v0.26 用户栈按需生长 ----
cmd "run deep"       "run deep
"      "\[deep\] pid=.* recursing 12\*1KB on a 4KB start stack" "\[stack\] grow pid=" "\[deep\] survived 12KB recursion via stack growth" "\[shell\] 'deep' exited code=0"
# ---- v0.29 回归盲区补格：已生长栈 × fork / exec 组合 ----
cmd "run deepfork"   "run deepfork
"      "\[deepfork\] pid=.* stack grown ~12KB, forking" "\[deepfork\] CHILD pid=.* inherited grown stack" "\[deepfork\] CHILD pid=.* grew beyond inherited stack" "\[deepfork\] PARENT pid=.* waited child=" "\[deepfork\] fork-of-grown-stack OK" "\[shell\] 'deepfork' exited code=0"
cmd "run deepexec"   "run deepexec
"      "\[deepexec\] pid=.* stack grown, exec'ing hello from depth" "Hello from 'hello' app! pid=" "\[shell\] 'deepexec' exited code=0"
# ---- v0.26#2 用户堆（brk/sbrk）：扩展/写入校验/收缩复用/bump alloc ----
cmd "run heapdemo"   "run heapdemo
"      "\[heapdemo\] initial brk=0x801a4000" "\[heapdemo\] sbrk(4096) old=0x801a4000" "\[heapdemo\] 4KB page write+verify OK" "\[heapdemo\] 16KB write+verify OK" "\[heapdemo\] shrink+reuse write+verify OK" "\[heapdemo\] bump alloc 3 blocks write+verify OK" "\[heapdemo\] survived heap brk/sbrk demo" "\[shell\] 'heapdemo' exited code=0"
# ---- v0.26#3 ELF 加载去上限：>64KB 大 ELF（旧 32KB/8 帧上限会拒绝） ----
cmd "run bigdemo"    "run bigdemo
"      "\[bigdemo\] pid=.* blob=70KB size=70000" "\[bigdemo\] 70KB write+verify sum=" "\[bigdemo\] survived big-ELF load" "\[shell\] 'bigdemo' exited code=0"
# ---- v0.27 工具链自举：cc500 编译自身两次，P1==P2 逐字节一致（写-编-跑闭环） ----
cmd "ccboot 自举"     "ccboot
"      "cc500: compiled OK" "\[ccboot\] byte-identical PASS"
# ---- v0.14 文件系统增强：shell 目录命令 + fsdemo ----
# 注意：QEMU HMP sendkey 不支持 '/'（斜杠会静默丢弃），此处用平铺名；
# 带斜杠路径的交互验证走串口通道（test_serial.sh）。
cmd "mkdir 目录"     "mkdir dir1
"      "\[shell\] mkdir 'dir1' -> "
cmd "ls 子目录"      "ls dir1
"      "\[ls\] dir1:"
cmd "rmdir 目录"     "rmdir dir1
"      "\[shell\] rmdir 'dir1' -> 0"
cmd "run fsdemo"     "run fsdemo
"      "\[fsdemo\] mkdir /etc -> " "\[fsdemo\] seek(5) read '8080" "\[fsdemo\] big.bin 100000B indirect spot-check OK" "\[fsdemo\] done" "\[shell\] 'fsdemo' exited code=0"
# ---- v0.15 wait 语义：wait(-1) 任意子进程 + exec 失败反馈 ----
cmd "run waitdemo"   "run waitdemo
"      "\[waitdemo\] parent pid=[0-9][0-9]* forked" "\[waitdemo\] wait any -> pid=[0-9][0-9]* code=7" "\[waitdemo\] wait any -> pid=[0-9][0-9]* code=9" "\[waitdemo\] wait any -> pid=[0-9][0-9]* code=11" "\[waitdemo\] verify OK" "\[waitdemo\] final wait any -> 4294967295" "\[waitdemo\] done" "\[shell\] 'waitdemo' exited code=0"
cmd "exec 失败反馈"   "exec nosuchprog
"      "\[exec\] FAILED to exec '"
# ---- v0.16 单行结构化自检（agent 可 grep 一行确认全量通过） ----
# v0.21：第 6 项为内核自审计（帧配平/堆完整性/信号量守恒/PCB 状态机）
cmd "selftest 自检"   "selftest
"      "\[selftest\] audit=0" "\[audit\] mem ok" "\[audit\] heap ok" "\[audit\] sched ok" "\[audit\] sem ok" "\[selftest\] PASS (6 checks)"
# ---- v0.17 syscall 边界校验：内核指针全部被拒 ----
cmd "run abuse"       "run abuse
"      "\[abuse\] write buf@0xB8000 -> 4294967295" "\[abuse\] verify OK"

# 等待剩余时间（让后台 sem/msg/fs 演示继续输出），随后收尾
END=$((QSTART + DURATION))
while kill -0 "$QPID" 2>/dev/null && [ "$SECONDS" -lt "$END" ]; do
    sleep 0.25
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

if [ ! -s "$LOG" ]; then
    echo "[FAIL] 未生成串口日志（内核可能没启动）"
    exit 1
fi
echo "      日志已生成 ($(wc -l < "$LOG") 行)"

echo "== [4/4] 校验串口日志关键里程碑 =="
FAIL=0
check() {   # check "<说明>" "<正则>"
    if grep -q "$2" "$LOG"; then
        echo "[ok]   $1"
    else
        echo "[FAIL] 缺少 $1 (匹配: $2)"
        FAIL=$((FAIL + 1))
    fi
}

# ---- v0.1 ~ v0.5 基础 ----
check "进程创建"          "creating processes"
check "spawn procA"       "spawn pid=1 name=procA"
check "spawn procB"       "spawn pid=2 name=procB"
check "spawn procSemA"    "spawn pid=3 name=procSemA"
check "spawn procSemB"    "spawn pid=4 name=procSemB"
check "spawn procCrash"   "spawn pid=5 name=procCrash"
check "切入第一个进程"      "start -> pid=1 name=procA"
check "procA 启动"        "procA started, pid="
check "procB 启动"        "procB started, pid="
check "procA 抢占打印"     "\[A\] tick="
check "procB 抢占打印"     "\[B\] tick="
check "进程 A sleep"      "sleep pid=1"
check "进程 B sleep"      "sleep pid=2"
check "阻塞进程被唤醒"      "wake pid="
check "crash 演示启动"     "crash demo: writing kernel memory"
check "crash 进程被终止"    "kill pid=5"
check "僵尸被回收"         "reap pid=5"
# ---- v0.6 IPC/同步 ----
check "信号量创建"         "\[sem\] create id=1"
check "sem 等待阻塞"       "sem\] wait.*block"
check "sem 信号唤醒"       "sem\] signal.*wake"
check "共享内存页映射"      "sem\] shmem slot=0"
check "rendezvous 会合"    "rendezvous done"
check "A 互斥自增"         "\[SA\] locked cnt="
check "B 互斥自增"         "\[SB\] locked cnt="
check "sem 演示完成"       "\[SB\] done"
# ---- v0.7 消息队列 ----
check "msg 队列创建"       "msg\] create id=1"
check "msg 消费者阻塞"     "msg\] recv.*block"
check "msg 生产者阻塞"     "msg\] send.*block"
check "msg 生产者被唤醒"    "msg\] recv.*wake producer"
check "msg 消费者拿到消息"  "\[MC\] got val="
check "msg 生产者发送"     "\[MP\] sent val="
check "msg 演示完成"       "\[MC\] done"
# ---- v0.8 文件系统 ----
check "spawn procFSA"     "spawn pid=8 name=procFSA"
check "spawn procFSB"     "spawn pid=9 name=procFSB"
check "内存盘初始化"       "\[storage\] ramdisk 256 blocks"
check "fs 创建 hello.txt" "\[fs\] create 'hello.txt'"
check "fs 打开写模式"      "\[fs\] open fd=1 'hello.txt' inode=.* mode=1"
check "fs 跨块写入"       "\[fs\] write fd=1"
check "fs 打开读模式"      "\[fs\] open fd=2 'hello.txt' inode=.* mode=0"
check "fs 读回校验通过"    "\[FA\] verify OK"
check "fs 创建 alpha.txt" "\[fs\] create 'alpha.txt'"
check "fs 创建 beta.txt"  "\[fs\] create 'beta.txt'"
check "v0.31 per-process fd 隔离" "\[fs\] open fd=1 'alpha.txt' inode=.* mode=1"
check "ls 列出 alpha"     "\[ls\]   alpha.txt"
check "ls 列出 beta"      "\[ls\]   beta.txt"
check "ls 演示完成"        "\[FL\] ls done"
# ---- v0.9 ELF 加载 + Shell ----
check "initramfs 写入 motd"   "\[ramdisk\] 'motd'"
check "initramfs 写入 hello"  "\[ramdisk\] 'hello'"
check "initramfs 写入 echo"   "\[ramdisk\] 'echo'"
check "initramfs 写入 crash"  "\[ramdisk\] 'crash'"
check "initramfs 写入 shell"  "\[ramdisk\] 'shell'"
check "内核加载 shell ELF"    "\[elf\] 'shell' loaded"
check "shell 进程创建"        "spawn_at pid=10 name=shell"
check "shell 提示符"          "mini-os\$ "
check "readline 阻塞"         "readline pid=10 -> block"
# ---- v0.11 每进程地址空间 ----
check "initramfs 写入 isol"   "\[ramdisk\] 'isol'"
check "隔离实例映射私有页"      "\[vm\] map_page pid=.* addr=80050000"
check "隔离演示通过"           "ISOLATED OK"
# 两个实例把同一虚拟地址映射到不同物理页（每进程地址空间生效的铁证）
ISOL_PHYS=$(grep -o '\[vm\] map_page pid=[0-9]* addr=80050000 phys=[0-9a-f]*' "$LOG" \
            | awk '{print $NF}' | sort -u | wc -l)
if [ "$ISOL_PHYS" -ge 2 ]; then
    echo "[ok]   隔离实例落到 $ISOL_PHYS 个不同物理页"
else
    echo "[FAIL] 隔离实例未落到不同物理页（仅 $ISOL_PHYS 个）"
    FAIL=$((FAIL + 1))
fi
# ---- v0.12 fork / exec / argv ----
check "initramfs 写入 forkdemo" "\[ramdisk\] 'forkdemo'"
check "initramfs 写入 args"     "\[ramdisk\] 'args'"
check "fork 父子分叉"            "\[fork\] pid=.* -> child="
check "fork 父进程拿子 pid"      "\[fork\] PARENT pid=.* fork returned child="
check "fork 子进程返回 0"        "\[fork\] CHILD pid=.* fork returned 0"
check "fork 深拷贝隔离"          "\[fork\] pid=.* ISOLATED OK"
check "exec 镜像替换"            "\[exec\] pid=.* -> 'args'"
check "argv 参数传递"            "\[args\] pid=.* argc=4"
check "argv[1] 内容"             "\[args\] argv\[1\]='alpha'"
check "exec 退出码"              "\[shell\] 'args' exited code=0"
# ---- v0.13 栈守卫页 ----
check "initramfs 写入 stackovf" "\[ramdisk\] 'stackovf'"
check "stackovf 启动"           "\[stackovf\] pid=.* starting"
check "栈溢出被检测"             "\[user\] STACK OVERFLOW pid="
check "stackovf 被终止"          "\[sched\] kill pid=.* name=stackovf"
# ---- v0.26 用户栈按需生长 ----
check "initramfs 写入 deep"     "\[ramdisk\] 'deep'"
check "deep 栈按需生长"          "\[stack\] grow pid=.* pages="
check "deep 存活"                "\[deep\] survived 12KB recursion via stack growth"
# ---- v0.29 回归盲区：已生长栈 × fork / exec 组合 ----
check "initramfs 写入 deepfork"  "\[ramdisk\] 'deepfork'"
check "deepfork 子继承已生长栈"   "\[deepfork\] CHILD pid=.* inherited grown stack"
check "deepfork 子超越继承栈"     "\[deepfork\] CHILD pid=.* grew beyond inherited stack"
check "deepfork 组合通过"         "\[deepfork\] fork-of-grown-stack OK"
check "initramfs 写入 deepexec"  "\[ramdisk\] 'deepexec'"
check "deepexec exec 后 hello 运行" "Hello from 'hello' app! pid="
# ---- v0.26#2 用户堆（brk/sbrk） ----
check "initramfs 写入 heapdemo" "\[ramdisk\] 'heapdemo'"
check "brk 起始日志"             "\[heap\] brk pid=.* 801a4000 -> 801a5000"
check "brk 扩 16KB 日志"         "\[heap\] brk pid=.* 801a5000 -> 801a9000"
check "brk 收缩保留映射"         "\[heap\] brk pid=.* 801a9000 -> 801a7000"
check "heapdemo 存活"            "\[heapdemo\] survived heap brk/sbrk demo"
# ---- v0.26#3 ELF 加载去上限：>64KB 大 ELF ----
check "initramfs 写入 bigdemo"   "\[ramdisk\] 'bigdemo'"
check "大 ELF 加载日志"           "\[elf\] 'bigdemo' loaded"
check "bigdemo 帧数超旧上限"      "\[elf\] 'bigdemo' loaded.*[0-9][0-9]* frames"
# ---- v0.27 工具链自举 ----
check "initramfs 写入 cc500"     "\[ramdisk\] 'cc500'"
check "initramfs 写入 cc500.c"   "\[ramdisk\] 'cc500.c'"
check "cc500 编译自身成 P1"       "cc500: compiled OK"
check "P1 被加载运行"             "\[elf\] '/out.elf' loaded.* entry=800a0054"
check "cc500 编译退出码"          "name=cc500 code=0"
check "P1 编译退出码"             "name=/out.elf code=0"
check "自举闭环 PASS"            "\[ccboot\] byte-identical PASS"
# ---- v0.14 文件系统增强 ----
check "initramfs 写入 fsdemo"   "\[ramdisk\] 'fsdemo'"
check "fsdemo 建目录 /etc"       "\[fsdemo\] mkdir /etc -> [0-9][0-9]*"
check "fsdemo 建子目录文件"      "\[fsdemo\] create /etc/sub/notes.txt -> "
check "fsdemo 追加写"            "\[fsdemo\] write 'host=0.0.0.0"
check "ls 显示目录类型标记"      "\[ls\]   etc/"
check "fsdemo seek 读回 8080"   "\[fsdemo\] seek(5) read '8080"
check "fsdemo 间接块大文件"      "\[fsdemo\] big.bin 100000B indirect spot-check OK"
check "fsdemo 拒绝删非空目录"    "\[fsdemo\] rmdir /etc -> 4294967295"
check "fsdemo 演示完成"          "\[fsdemo\] done"
check "shell mkdir 命令"         "\[shell\] mkdir 'dir1' -> "
# ---- v0.15 wait 语义 ----
check "initramfs 写入 waitdemo"  "\[ramdisk\] 'waitdemo'"
check "waitdemo 父进程 fork"     "\[waitdemo\] parent pid=[0-9][0-9]* forked"
check "wait 任意回收 code=7"     "\[waitdemo\] wait any -> pid=[0-9][0-9]* code=7"
check "wait 任意回收 code=9"     "\[waitdemo\] wait any -> pid=[0-9][0-9]* code=9"
check "wait 任意回收 code=11"    "\[waitdemo\] wait any -> pid=[0-9][0-9]* code=11"
check "waitdemo 校验通过"        "\[waitdemo\] verify OK"
check "wait 无子进程返回 -1"      "\[waitdemo\] final wait any -> 4294967295"
check "exec 失败反馈"            "\[exec\] FAILED to exec '"
check "wait 内核日志 reaped"      "\[user\] wait any -> pid="
# ---- 通用 ----
check "idle 状态行心跳"     "alive="
check "定时器心跳正常"      "ticks="

echo
FAIL=$((FAIL + INTERACTIVE_FAIL))
if [ "$FAIL" -eq 0 ]; then
    echo "QEMU 回归测试通过"
    exit 0
else
    echo "QEMU 回归测试失败: $FAIL 项未通过（详见上方）"
    exit 1
fi
