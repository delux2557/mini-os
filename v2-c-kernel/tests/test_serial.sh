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
# ⚠ 备注：本脚本观测打点用 GNU date 的 `+%s%3N`（毫秒时间戳）。CI=ubuntu（GNU date）无碍；
#   本地 mac 的 BSD date 不支持 %3N，打点处已做 `|| echo 0` 兜底（耗时记 0，不崩、不误判）。
for c in qemu-system-i386; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

LOG="$BUILD/serial_term.log"
TIN="$BUILD/term_in.fifo"
TOUT="$BUILD/term_out.fifo"
# ---- 观测打点（Commit 2，纯观测→RR 基线）：assert_timing_serial.tsv ----
# 每个等断断言(含 wait_for)恰打一行 TSV：`断言名 \t 耗时ms \t ok|timeout`。
# 断言名=RR 基线 key，写前 sed 转义空白/tab/换行防列错位；改名即新基线（见 docs 维护规则）。
# 文件按脚本分名（_serial/_socket/_qemu），防 test job 串行 make test 同 BUILD 目录互相覆盖。
TSV="$BUILD/assert_timing_serial.tsv"
rm -f "$TSV"
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
# 观测打点：每断言恰打一行 TSV（断言名已 sed 转义 tab/换行，防列错位）。
# 耗时 ~ 250ms 轮询粒度（0.25s sleep），非真毫秒；P50/P95 与 timeout 判定不受影响。
pop_ts() {  # pop_ts <tsv> <断言名> <耗时ms> <ok|timeout>
    local pdesc
    pdesc=$(printf '%s' "$2" | sed 's/[\t\n]/_/g')   # 断言名转义 tab/换行
    printf '%s\t%s\t%s\n' "$pdesc" "$3" "$4" >> "$1"
}
wait_for() {   # wait_for <说明> <正则> [超时秒]
    local desc="$1" re="$2" tmo="${3:-8}" i t0 t1
    t0=$(date +%s%3N 2>/dev/null || echo 0)          # GNU date；mac 兜底 0
    for ((i = 0; i < tmo * 4; i++)); do
        grep -aq "$re" "$LOG" 2>/dev/null && break
        sleep 0.25
    done
    t1=$(date +%s%3N 2>/dev/null || echo 0)
    if [ "$i" -lt $((tmo * 4)) ]; then
        pop_ts "$TSV" "$desc" "$((t1 - t0))" ok
        echo "[ok]   $desc"
        return 0
    fi
    pop_ts "$TSV" "$desc" "$((t1 - t0))" timeout
    echo "[FAIL] $desc (缺: $re)"
    echo "  >> 现场（LOG 尾 ~20 行）："
    tail -n 20 "$LOG" 2>/dev/null | sed 's/^/      /'
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
# ---- v1.5 P2 编译硬化：栈金丝雀（与守卫页互补的"函数返回前"拦截）----
# 该 demo 走 canary 路径：进程打印 [CRIT] 后 sys_exit(1)，内核存活、无 [FATAL]。
# 随后的 deep/... 命令继续可执行，即为"整机心跳持续 / 未停整机"的隐式断言。
send "run cansmash"
wait_for "cansmash 启动"       "\[cansmash\] starting stack smash"
wait_for "cansmash 触发金丝雀"  "\[CRIT\] stack smashing detected"
wait_for "cansmash 退出"       "'cansmash' exited code="
# ---- v0.34 BUG-058 per-process syscall 掩码（最小权限，seccomp 教学版） ----
# 单向收窄：受限后 fs 写面/网络 syscall 返回 -1，生存项仍活，sys_limit(0,0) 无法放宽。
send "run sandboxdemo"
wait_for "沙盒演示启动"        "\[sandboxdemo\] mask demo start"
wait_for "沙盒受限后存活"      "\[sandboxdemo\] still alive pid="
wait_for "沙盒演示通过"        "\[sandboxdemo\] verify OK"
wait_for "沙盒演示退出码"      "'sandboxdemo' exited code="
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
# ---- v0.35（Red Team F1/F2 修复）长合法名(16..23)可加载运行；失败显式、绝不伪装 PASS ----
# F1：旧 16B 按名/按路径加载缓冲会把 16~23 字符合法程序名静默截断撞前名前缀、误加载错误程序。
#     现统一 64B + copyin_str_full（超长显式失败）。正向断言：20 字符源名 + 18 字符加载名真正跑通。
#     （加载名 basename 取 22 字符含 .elf，≤ FS_MAX_NAME=24；若取 23 字符会被 cc500 拒为超限，
#      与 F1 无关，那是 fs 单分量上限校验的正常拒绝。）
#     ⚠ 两点工程约束：
#       a) writefile 单行有 128B 截断，源名 22B 时命令行须精简（源码刻意短小，含 syscall3 声明总长 ≤ ~120B）；
#       b) 断言必须按"本轮(SN0)之后的新行"切片——`cc500: compiled OK` 在先前 cc500 自举用例中已出现，
#          整日志 wait_for 会虚假命中；compiled OK → loaded → 运行输出 → PASS 用轮询逐段验证。
# ⚠ run 计时语义（插曲 2，类型 II 断言根修）：`run=` 为 10ms tick 粒度（wall 100Hz），快速 TCG / tick
#  边界取整下 **run=0ms 是合法快执行**，不能作为"未运行"判据。程序真实性由 `[elf] ... loaded` +
#  程序自打印（F1ok）承担；BUG-066"假 PASS"防回归由下方 F2 负向断言独立覆盖——故 run>0 为纯冗余过严，
#  断言收敛为 `run=[0-9]+`（只需声明本行是 PASS 行带 run 字段，不校对时序）。
# F2：cmd_ccrun 旧用 uint32_t 收 sys_spawn_file 返回值，失败 -1 化 4294967295 使 `pid<=0` 恒假、
#     静默落入 sys_wait(-1) → code=0 → 打印假 "PASS (run=0ms)"。现改有符号判败；下方做负向断言。
SN0=$(wc -l < "$LOG")
send 'writefile /aaaaaaaaaaaaaaaaaaaa.c int syscall3(int n,int a,int b,int c);int main(){syscall3(1,"F1ok\x0a",0,0);return 0;}'
wait_for "长 20 字符源名写入"    "\[writefile\] '/aaaaaaaaaaaaaaaaaaaa.c' wrote"
send 'ccrun /aaaaaaaaaaaaaaaaaaaa.c /mmmmmmmmmmmmmmmmmm.elf'
# 轮询等待本轮切片出现 编译成功+加载+运行输出+PASS且run>0 四连，再逐段断言
SLICE() { tail -n "+$((SN0+1))" "$LOG"; }
LSEG=""
for _ in $(seq 1 16); do LSEG=$(SLICE); \
    [ -n "$(printf '%s\n' "$LSEG" | grep -a "cc500: compiled OK")" ] && \
    [ -n "$(printf '%s\n' "$LSEG" | grep -a "\[elf\] '/mmmmmmmmmmmmmmmmmm.elf' loaded")" ] && \
    [ -n "$(printf '%s\n' "$LSEG" | grep -aF "F1ok")" ] && \
    [ -n "$(printf '%s\n' "$LSEG" | grep -aE "PASS \(compile=[0-9]+ms run=[0-9]+")" ] && break; sleep 0.5; done
printf '%s\n' "$LSEG" | grep -a "cc500: compiled OK" >/dev/null \
  && echo "[ok]   长 20 字符源名可编译" || { echo "[FAIL] 长 20 字符源名可编译"; FAIL=$((FAIL+1)); }
printf '%s\n' "$LSEG" | grep -a "\[elf\] '/mmmmmmmmmmmmmmmmmm.elf' loaded" >/dev/null \
  && echo "[ok]   长 18 字符加载名成功加载" || { echo "[FAIL] 长 18 字符加载名成功加载"; FAIL=$((FAIL+1)); }
printf '%s\n' "$LSEG" | grep -aF "F1ok" >/dev/null \
  && echo "[ok]   长名程序真实运行" || { echo "[FAIL] 长名程序真实运行"; FAIL=$((FAIL+1)); }
printf '%s\n' "$LSEG" | grep -aE "PASS \(compile=[0-9]+ms run=[0-9]+" >/dev/null \
  && echo "[ok]   长名运行 PASS 行存在" || { echo "[FAIL] 长名运行 PASS 行缺失"; FAIL=$((FAIL+1)); }
# F2 负向：失败的 ccrun 必须显式编译失败，且绝不追加假 PASS（防 run=0ms 假阳性归来）。
SN1=$(wc -l < "$LOG")
send 'ccrun /no_such_src_xyz.c /no_out.elf'
wait_for "不存在的源显式编译失败" "\[ccrun\] compile FAIL code="
if tail -n "+$((SN1+1))" "$LOG" | grep -aqE "exited code=0 PASS"; then
    echo "[FAIL] 失败命令却出现 PASS（F2 假阳性未消除）"; FAIL=$((FAIL+1))
else
    echo "[ok]   失败命令未出现 PASS（F2 判败修复确认）"
fi
# ---- 红队二轮（ERRATA-R2）G1 / D4 回归：BUG-067 / BUG-068 ----
# 载荷 cc500 方言合规：纯 ASCII、无数组/for/break/cast、无 unary 负号（-1 用 0-1 表达式，
# 否则 "-" 在字面量位报 error）；指针经 uint32_t 形参隐式转换。⚠ 字符串字面量
# （如 "G1ret-NEG"、"D4EVIL"）会先被串口回显进日志，故断言一律取 ccrun/程序运行段
# （SN 行号切片：SN 在 writefile 回显完成后、发 ccrun 前落片），排除回显假阳性（沿用 F1 切片模式）。
#
# G1（RBT-2026-013，BUG-067）：sys_readline(max=0) 须立即返 -1、绝不阻塞。
#     传合法指针 + max=0 且不注入任何键盘输入：若旧实现仍阻塞/越界写，程序永远到不了
#     打印段，切片断言超时判败（同时覆盖"返 -1"与"不阻塞"两个性质）。
send 'writefile <<EOF /g1_rb.c'
send 'int syscall3(int n,int a,int b,int c);'
send 'int main(){'
send 'int ret=syscall3(20,"x",0,0);'
send 'if(ret==0-1){ syscall3(1,"G1ret-NEG\x0a",0,0);}'
send 'if(ret!=0-1){ syscall3(1,"G1ret-OTHER\x0a",0,0);}'
send 'return 0;'
send '}'
send 'EOF'
wait_for "G1 源码入账"  "\[writefile\] '/g1_rb.c' wrote .* (heredoc)"
SN_G1=$(wc -l < "$LOG")
send 'ccrun /g1_rb.c /g1_rb.elf'
G1SLICE=""
for _ in $(seq 1 16); do G1SLICE=$(tail -n "+$((SN_G1+1))" "$LOG"); \
    [ -n "$(printf '%s\n' "$G1SLICE" | grep -aF "G1ret-NEG")" ] && break; sleep 0.5; done
printf '%s\n' "$G1SLICE" | grep -aF "G1ret-NEG" >/dev/null \
  && echo "[ok]   G1 readline(max=0) 立即返 -1、不阻塞" \
  || { echo "[FAIL] G1 readline(max=0) 未返 -1 或阻塞"; FAIL=$((FAIL+1)); }
# D4（RBT-2026-014，BUG-068）：删除仍被打开的 fd 须回收；旧 fd 写落新文件必须失败。
#     流程：create A -> open fd=1(A,wr) -> write OLD -> rm A（触发 revoke fd=1）
#           -> create B -> open fd=2(B,wr) -> write BOK -> 经悬垂 fd1 写 D4EVIL。
#     断言：a) 出现 [fs] revoke 日志；b) fd1 写返 -1（D4fd1-NEG）；c) cat /dB 仅含 BOK、绝无 D4EVIL。
send 'writefile <<EOF /d4_rb.c'
send 'int syscall3(int n,int a,int b,int c);'
send 'int main(){'
send 'int r;'
send 'syscall3(13,"/dA",0,0);'
send 'r=syscall3(14,1,"/dA",1);'
send 'syscall3(15,1,"OLD",3);'
send 'syscall3(19,"/dA",0,0);'
send 'syscall3(13,"/dB",0,0);'
send 'r=syscall3(14,2,"/dB",1);'
send 'syscall3(15,2,"BOK",3);'
send 'r=syscall3(15,1,"D4EVIL",6);'
send 'if(r==0-1){ syscall3(1,"D4fd1-NEG\x0a",0,0);}'
send 'if(r!=0-1){ syscall3(1,"D4fd1-OKbad\x0a",0,0);}'
send 'return 0;'
send '}'
send 'EOF'
wait_for "D4 源码入账"  "\[writefile\] '/d4_rb.c' wrote .* (heredoc)"
SN_D4=$(wc -l < "$LOG")
send 'ccrun /d4_rb.c /d4_rb.elf'
D4SLICE=""
for _ in $(seq 1 16); do D4SLICE=$(tail -n "+$((SN_D4+1))" "$LOG"); \
    [ -n "$(printf '%s\n' "$D4SLICE" | grep -aF "D4fd1-NEG")" ] && \
    [ -n "$(printf '%s\n' "$D4SLICE" | grep -aF "[fs] revoke fd=")" ] && break; sleep 0.5; done
printf '%s\n' "$D4SLICE" | grep -aF "[fs] revoke fd=" >/dev/null \
  && echo "[ok]   D4 revoke 日志出现" || { echo "[FAIL] D4 无 revoke 日志"; FAIL=$((FAIL+1)); }
printf '%s\n' "$D4SLICE" | grep -aF "D4fd1-NEG" >/dev/null \
  && echo "[ok]   D4 悬垂 fd 写返 -1" || { echo "[FAIL] D4 悬垂 fd 写未返 -1"; FAIL=$((FAIL+1)); }
# c) cat /dB 复核：内容应为 BOK，绝不含 D4EVIL（若旧 fd 写落新文件则 /dB 会含 D4EVIL）。
SN_CAT=$(wc -l < "$LOG")
send 'cat /dB'
CATSLICE=""
for _ in $(seq 1 10); do CATSLICE=$(tail -n "+$((SN_CAT+1))" "$LOG"); \
    [ -n "$(printf '%s\n' "$CATSLICE" | grep -aF "BOK")" ] && break; sleep 0.4; done
printf '%s\n' "$CATSLICE" | grep -aF "BOK" >/dev/null \
  && echo "[ok]   /dB 内容为 BOK" || { echo "[FAIL] /dB 未含 BOK"; FAIL=$((FAIL+1)); }
if printf '%s\n' "$CATSLICE" | grep -aq "D4EVIL"; then
    echo "[FAIL] 悬垂 fd 字节落入新文件 /dB（D4 未修复）"; FAIL=$((FAIL+1))
else
    echo "[ok]   /dB 不含悬垂 fd 写入（无 D4EVIL）"
fi
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
