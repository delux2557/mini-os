#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_serial.sh
# v0.10 串口终端回归：模拟"外部 agent 经 QEMU 串口终端驱动 mini-os shell"。
#   - QEMU 以 `-serial stdio` 运行，串口即双向终端（FIFO 管道模拟 agent 通道）
#   - agent 向串口发送命令 -> 内核 IRQ4 接收 -> 行缓冲 -> shell 执行 -> 输出回串口
#   - 校验：命令回显 + 各命令输出（help/ls/cat motd/run hello/run echo/run crash）
# 与 qemu_regression.sh（键盘 sendkey 路径）互补，验证"终端通道"而非"键盘通道"。
set -u
cd "$(dirname "$0")/.." || exit 1

LOG="build/serial_term.log"
TIN="build/term_in.fifo"
TOUT="build/term_out.fifo"
QPID=""; CAT_PID=""

cleanup() {
    exec 9>&- 2>/dev/null || true        # 关闭 FIFO 写端（QEMU 串口 stdin EOF）
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true
    rm -f "$TIN" "$TOUT"
}
trap cleanup EXIT

echo "== [1/3] 构建内核 =="
make >/dev/null 2>&1 || { echo "[FAIL] 内核构建失败"; exit 1; }
echo "      构建完成"

echo "== [2/3] QEMU -serial stdio 串口终端（FIFO 模拟 agent 通道） =="
rm -f "$LOG" "$TIN" "$TOUT"
mkfifo "$TIN" "$TOUT"
cat "$TOUT" > "$LOG" & CAT_PID=$!      # 串口输出 -> 日志（可轮询断言）
qemu-system-i386 -kernel build/kernel.elf -display none -vga std \
    -no-reboot -no-shutdown -m 64 -serial stdio -monitor none \
    < "$TIN" > "$TOUT" 2>/dev/null &
QPID=$!
exec 9>"$TIN"                            # 保持写端打开，向串口发命令（固定 fd 9）

FAIL=0
wait_for() {   # wait_for <说明> <正则> [超时秒]
    local desc="$1" re="$2" tmo="${3:-8}" i
    for ((i = 0; i < tmo * 4; i++)); do
        grep -aq "$re" "$LOG" 2>/dev/null && { echo "[ok]   $desc"; return 0; }
        sleep 0.25
    done
    echo "[FAIL] $desc (缺: $re)"
    FAIL=$((FAIL + 1)); return 1
}
send() { printf '%s\n' "$1" >&9; sleep 0.3; }

# 等 shell 提示符出现（内核启动 + 加载 shell 完成）
wait_for "shell 提示符"        "mini-os\$ " 20

echo "== [3/3] agent 逐命令交互 =="
send "help"
wait_for "命令回显 help"       "help"
wait_for "help 输出"           "mini-os shell commands:"
send "ls"
wait_for "ls 输出"             "\[ls\] /:"
send "cat motd"
wait_for "cat motd 输出"       "Mini-OS v0.13: user stack guard pages"
send "run hello"
wait_for "run hello 输出"      "Hello from 'hello' app! pid="
wait_for "hello 退出码"        "'hello' exited code=0"
send "run echo"
wait_for "echo 提示输入"       "type a line and press Enter"
send "hi"
wait_for "echo 回显输入"       "\[echo\] got 2 bytes: \[hi\]"
send "run crash"
wait_for "crash 写入"          "\[crash\] writing kernel memory"
wait_for "crash 被隔离退出"    "'crash' exited code="
send "run isol"
wait_for "isol 映射私有页"     "\[isol\] pid=.* map ok addr=0x80050000"
wait_for "isol 隔离通过"       "\[isol\] pid=.* ISOLATED OK"
wait_for "isol 退出码"        "'isol' exited code=0"
# ---- v0.12 fork / exec / argv ----
send "run forkdemo"
wait_for "fork 父子分叉"       "\[fork\] pid=.* -> child="
wait_for "fork 父进程拿子 pid" "\[fork\] PARENT pid=.* fork returned child="
wait_for "fork 子进程返回 0"   "\[fork\] CHILD pid=.* fork returned 0"
wait_for "fork 隔离通过"       "\[fork\] pid=.* ISOLATED OK"
wait_for "forkdemo 退出码"    "'forkdemo' exited code=0"
send "exec args hello world"
wait_for "exec 镜像替换"       "\[exec\] pid=.* -> 'args'"
wait_for "exec argv"          "\[args\] pid=.* argc=3"
wait_for "exec argv[1]"       "\[args\] argv\[1\]='hello'"
wait_for "exec 退出码"        "'args' exited code=0"
# ---- v0.13 栈守卫页 ----
send "run stackovf"
wait_for "stackovf 启动"       "\[stackovf\] pid=.* starting"
wait_for "栈溢出被检测"        "\[user\] STACK OVERFLOW pid="
wait_for "stackovf 被终止"    "'stackovf' exited code="

# 稳定后收尾（让串口缓冲落盘）
sleep 1
exec 9>&-                                # 关闭写端
kill "$QPID" 2>/dev/null || true; QPID=""
wait "$CAT_PID" 2>/dev/null || true; CAT_PID=""
rm -f "$TIN" "$TOUT"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "串口终端回归通过（agent 可经串口驱动 shell）"
    exit 0
else
    echo "串口终端回归失败: $FAIL 项未通过"
    exit 1
fi
