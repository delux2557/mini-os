#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_serial.sh
# v0.10 串口终端回归：模拟"外部 agent 经 QEMU 串口终端驱动 mini-os shell"。
#   - QEMU 以 `-serial stdio` 运行，串口即双向终端（FIFO 管道模拟 agent 通道）
#   - agent 向串口发送命令 -> 内核 IRQ4 接收 -> 行缓冲 -> shell 执行 -> 输出回串口
#   - 校验：命令回显 + 各命令输出（help/ls/cat motd/run hello/run echo/run crash）
# 与 qemu_regression.sh（键盘 sendkey 路径）互补，验证"终端通道"而非"键盘通道"。
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh
# v0.33 harness 约定：exit 0=全绿 / 1=断言失败 / 2=环境或依赖缺失
for c in qemu-system-i386; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

LOG="$BUILD/serial_term.log"
TIN="$BUILD/term_in.fifo"
TOUT="$BUILD/term_out.fifo"
QPID=""; CAT_PID=""

cleanup() {
    exec 9>&- 2>/dev/null || true        # 关闭 FIFO 写端（QEMU 串口 stdin EOF）
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true
    rm -f "$TIN" "$TOUT"
}
trap cleanup EXIT

echo "== [1/3] 构建内核 =="
make BUILD="$BUILD" >/dev/null 2>&1 || { echo "[FAIL] 内核构建失败"; exit 1; }
echo "      构建完成"

echo "== [2/3] QEMU -serial stdio 串口终端（FIFO 模拟 agent 通道） =="
rm -f "$LOG" "$TIN" "$TOUT"
mkfifo "$TIN" "$TOUT"
cat "$TOUT" > "$LOG" & CAT_PID=$!      # 串口输出 -> 日志（可轮询断言）
qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none -vga std \
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
wait_for "cat motd 输出"       "Mini-OS v0.33: toolchain self-host (cc500 compiles itself)"
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
# ---- v0.26 用户栈按需生长 ----
send "run deep"
wait_for "deep 开始递归"       "\[deep\] pid=.* recursing 12\*1KB on a 4KB start stack"
wait_for "栈按需生长发生"      "\[stack\] grow pid="
wait_for "deep 存活"           "\[deep\] survived 12KB recursion via stack growth"
wait_for "deep 退出码"        "'deep' exited code=0"
# ---- v0.29 回归盲区补格：已生长栈 × fork / exec 组合 ----
send "run deepfork"
wait_for "deepfork 父已生长栈"  "\[deepfork\] pid=.* stack grown ~12KB, forking"
wait_for "deepfork 子继承已生长栈" "\[deepfork\] CHILD pid=.* inherited grown stack"
wait_for "deepfork 子超越继承栈" "\[deepfork\] CHILD pid=.* grew beyond inherited stack"
wait_for "deepfork 组合通过"   "\[deepfork\] fork-of-grown-stack OK"
wait_for "deepfork 退出码"    "'deepfork' exited code=0"
send "run deepexec"
wait_for "deepexec 已生长栈 exec" "\[deepexec\] pid=.* stack grown, exec'ing hello from depth"
wait_for "deepexec exec 后 hello" "Hello from 'hello' app! pid="
wait_for "deepexec 退出码"    "'deepexec' exited code=0"
# ---- v0.26#2 用户堆（brk/sbrk） ----
send "run heapdemo"
wait_for "heapdemo 启动"       "\[heapdemo\] pid="
wait_for "brk 查询起点"        "\[heapdemo\] initial brk=0x801a4000"
wait_for "sbrk 扩展一页"       "\[heapdemo\] sbrk(4096) old=0x801a4000"
wait_for "4KB 页校验"          "\[heapdemo\] 4KB page write+verify OK"
wait_for "sbrk 扩 16KB"        "\[heapdemo\] sbrk(16384) old=0x801a5000"
wait_for "16KB 校验"           "\[heapdemo\] 16KB write+verify OK"
wait_for "收缩复用校验"        "\[heapdemo\] shrink+reuse write+verify OK"
wait_for "bump alloc 校验"     "\[heapdemo\] bump alloc 3 blocks write+verify OK"
wait_for "heapdemo 存活"       "\[heapdemo\] survived heap brk/sbrk demo"
wait_for "heapdemo 退出码"    "'heapdemo' exited code=0"
# ---- v0.26#3 ELF 加载去上限：>64KB 大 ELF（旧 32KB/8 帧上限会拒绝） ----
send "run bigdemo"
wait_for "bigdemo 启动"        "\[bigdemo\] pid=.* blob=70KB size=70000"
wait_for "bigdemo 填充校验"    "\[bigdemo\] 70KB write+verify sum="
wait_for "bigdemo 存活"        "\[bigdemo\] survived big-ELF load"
wait_for "bigdemo 退出码"     "'bigdemo' exited code=0"
# ---- v0.27 工具链自举：cc500 编译自身两次，P1==P2 逐字节一致（写-编-跑闭环） ----
send "ccboot"
wait_for "cc500 编译自身"       "cc500: compiled OK"
wait_for "cc500 编译退出码"     "name=cc500 code=0"
wait_for "P1 编译退出码"        "name=/out.elf code=0"
wait_for "自举闭环 PASS"        "\[ccboot\] byte-identical PASS"
# ---- v0.27b 写-编-跑（任意程序）：writefile 写源码 -> ccrun 编译并运行 ----
send 'writefile /hello.c int syscall3(int n,int a,int b,int c);int main(){syscall3(1,"hello, world\x0a",0,0);return 0;}'
wait_for "writefile 写源码"     "\[writefile\] '/hello.c' wrote [0-9][0-9]* bytes"
send "ccrun /hello.c /hello.elf"
wait_for "cc500 编译成功"       "cc500: compiled OK"
wait_for "编译产物被加载"        "\[elf\] '/hello.elf' loaded"
wait_for "ccrun 编译运行 PASS"  "\[ccrun\] '/hello.elf' exited code=0 PASS"
# ---- v1.4 heredoc 多行写入：writefile <<EOF /multi.c（逐行拼接，绕开单行 128B 截断） ----
send 'writefile <<EOF /multi.c'
send 'int syscall3(int n,int a,int b,int c);'
send 'int main(){'
send 'syscall3(1,"1234567890123456789012345678901234567890",0,0);'
send 'syscall3(1,"abcdefghijklmnopqrstuvwxyz-0123456789",0,0);'
send 'syscall3(1,"ok\x0a",3,0);'
send 'return 0;'
send '}'
send 'EOF'
wait_for "writefile heredoc 写多行" "\[writefile\] '/multi.c' wrote [1-9][0-9][0-9]* bytes (heredoc)"
send "ccrun /multi.c /multi.elf"
wait_for "heredoc 源码可编译"      "cc500: compiled OK"
wait_for "heredoc 源码可运行 PASS" "\[ccrun\] '/multi.elf' exited code=0 PASS"
# ---- v0.14 文件系统增强 ----
send "mkdir /sd1"
wait_for "mkdir 返回"          "\[shell\] mkdir '/sd1' -> "
send "ls /sd1"
wait_for "ls 子目录"           "\[ls\] /sd1:"
send "run fsdemo"
wait_for "fsdemo 建目录"       "\[fsdemo\] mkdir /etc -> "
wait_for "fsdemo 追加写"       "\[fsdemo\] write 'host=0.0.0.0"
wait_for "fsdemo seek 读回"    "\[fsdemo\] seek(5) read '8080"
wait_for "fsdemo seek 校验 OK" "host=0.0.0.0' -> OK"
wait_for "fsdemo 间接块"       "\[fsdemo\] big.bin 100000B indirect spot-check OK"
wait_for "fsdemo 完成"        "\[fsdemo\] done"
wait_for "fsdemo 退出码"      "'fsdemo' exited code=0"
send "rmdir /sd1"
wait_for "rmdir 返回 0"        "\[shell\] rmdir '/sd1' -> 0"
# ---- v0.15 wait 语义 ----
send "run waitdemo"
wait_for "waitdemo 父进程"     "\[waitdemo\] parent pid=.* forked"
wait_for "wait 任意回收"       "\[waitdemo\] wait any -> pid=.* code=7"
wait_for "wait 校验通过"       "\[waitdemo\] verify OK"
wait_for "wait 无子返回 -1"    "\[waitdemo\] final wait any -> 4294967295"
wait_for "waitdemo 完成"      "\[waitdemo\] done"
send "exec nosuchprog"
wait_for "exec 失败反馈"       "\[exec\] FAILED to exec '"
# ---- v0.16 单行结构化自检 ----
send "selftest"
wait_for "selftest 自检通过"   "\[selftest\] PASS (6 checks)"
# ---- v0.17 syscall 边界校验 ----
send "run abuse"
wait_for "abuse 内核指针被拒"  "\[abuse\] print@0x100000 -> 4294967295"
wait_for "abuse 校验通过"     "\[abuse\] verify OK"
# ---- BUG-056: 畸形大 p_memsz ELF 必须 -1 拒绝、不得整机 [FATAL] ----
send "run zbig"
wait_for "zbig 被 -1 拒绝"    "cannot load 'zbig'"

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
