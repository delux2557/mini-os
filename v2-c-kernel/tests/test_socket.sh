#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_socket.sh
# v0.31 socket 归属回归：串口驱动（可靠传 writefile 的 `/` 与 `()`）+ e1000 + 短租期 DHCP。
#   F-0a 退出泄漏：ring3 程序开 socket 不关即退出 -> 表槽永久失踪。修复=exit 回收。
#   F-0b 归属缺失：ring3 程序 close(id=0) 打死 DHCP 续约端点。修复=reserved + 隶属校验。
#   观测收口：kern_audit 增 [audit] netsock ok；case 30 表满打 [net] socket table full。
# 说明：攻击走 guest 内 cc500 写-编-跑（writefile + ccrun），证"任意 ring3 程序无需提权"。
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh
# v0.33 harness 约定：exit 0=全绿 / 1=断言失败 / 2=环境或依赖缺失（避免环境病伪装成代码病）
for c in qemu-system-i386 socat python3; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

LOG="$BUILD/socket.log"
TIN="$BUILD/socket_in.fifo"
TOUT="$BUILD/socket_out.fifo"
ECHO_LOG="$BUILD/socket_echo.log"
QPID=""; CAT_PID=""; ECHO_PID=""; RESTORED=0
FAIL=0

restore_kernel() {
    [ "$RESTORED" = 1 ] && return
    RESTORED=1
    make clean BUILD="$BUILD" >/dev/null 2>&1 && make BUILD="$BUILD" >/dev/null 2>&1 || true
}

cleanup() {
    exec 9>&- 2>/dev/null || true
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true
    [ -n "$ECHO_PID" ] && kill "$ECHO_PID" 2>/dev/null || true
    restore_kernel
    rm -f "$TIN" "$TOUT" "$ECHO_LOG"
}
trap cleanup EXIT

echo "== [1/4] 构建内核（DHCP_RENEW_SECS=2 短租期：供续约闭环观察） =="
make clean BUILD="$BUILD" >/dev/null 2>&1
if ! make DHCP_RENEW_SECS=2 BUILD="$BUILD" >/dev/null 2>&1; then
    echo "[FAIL] 内核构建失败"
    exit 1
fi
echo "      构建完成"

echo "== [2/4] 宿主 UDP echo 服务（0.0.0.0:7777，供 netping PONG） =="
if command -v python3 >/dev/null 2>&1; then
    python3 - >/dev/null 2>&1 <<'EOF' &
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', 7777))
while True:
    d, a = s.recvfrom(2048)
    s.sendto(b'PONG' + d, a)
EOF
    ECHO_PID=$!
    sleep 0.5
    echo "      echo server pid=$ECHO_PID"
else
    echo "[FAIL] 需要 python3 提供 UDP echo 服务"
    exit 1
fi

echo "== [3/4] QEMU 运行（e1000 + SLIRP + 串口终端） =="
rm -f "$LOG" "$TIN" "$TOUT"
mkfifo "$TIN" "$TOUT"
cat "$TOUT" > "$LOG" & CAT_PID=$!
qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none -vga std \
    -no-reboot -no-shutdown -m 64 -serial stdio -monitor none \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    < "$TIN" > "$TOUT" 2>/dev/null &
QPID=$!
exec 9>"$TIN"                        # 固定 fd 9 写串口

# 有序断言工具：从 start 行起匹配（保证"攻击之后"的时序）
wait_for() {   # wait_for <说明> <正则> [超时秒]
    local desc="$1" re="$2" tmo="${3:-10}" i
    for ((i = 0; i < tmo * 4; i++)); do
        grep -aq "$re" "$LOG" 2>/dev/null && { echo "[ok]   $desc"; return 0; }
        sleep 0.25
    done
    echo "[FAIL] $desc (缺: $re)"
    FAIL=$((FAIL + 1)); return 1
}
wait_after() { # wait_after <起始行> <说明> <正则> [超时秒]
    local start="$1" desc="$2" re="$3" tmo="${4:-10}" i
    for ((i = 0; i < tmo * 4; i++)); do
        if tail -n +$((start + 1)) "$LOG" 2>/dev/null | grep -q "$re"; then
            echo "[ok]   $desc"; return 0
        fi
        sleep 0.25
    done
    echo "[FAIL] $desc (缺: $re, from line $start)"
    FAIL=$((FAIL + 1)); return 1
}
send() { printf '%s\n' "$1" >&9; sleep 0.3; }

# 等引导 + DHCP 动态取 IP + shell 提示符
wait_for "shell 提示符"         "mini-os\$ " 25
wait_for "DHCP ACK 动态取 IP"   "\[dhcp\] .*ACK: ip [0-9].*gw [0-9]"

echo "== [4/4] socket 归属攻击回归 =="
# ---- 基线：短租期续约必须先通（DHCP 槽存活的前提） ----
wait_for "基线续约 RENEW"       "\[dhcp\] renew: sent RENEW (unicast)" 12
wait_for "基线续约 ACK"         "\[dhcp\] renew ACK: ip [0-9]\|\[dhcp\] rebind ACK: ip [0-9]" 8

# ---- F-0a：开 5 个 socket 不关即退出（表容量 4，DHCP 占 1，实际可开 3+） ----
send 'writefile /leak2.c int syscall3(int n,int a,int b,int c);int main(){int i;i=0;while(i<=4){syscall3(30,0,0,0);i=i+1;}return 0;}'
wait_for "writefile 写 leak2.c" "\[writefile\] '/leak2.c' wrote"
send "ccrun /leak2.c /leak2.elf"
wait_for "leak2 编译运行 PASS"  "\[ccrun\] '/leak2.elf' exited code=0 PASS" 20
wait_for "F-0a 退出回收 socket" "\[netsock\] close id=.* (proc .* exit cleanup)" 10

# 修复后：泄漏进程退出的 socket 已回收 -> netping 可新开 socket -> PONG
send "netping"
wait_for "F-0a 修复：泄漏后 netping 仍 PONG" "\[netping\] 10.0.2.2:7777 PONG" 15

# ---- F-0b：close(id=0)（DHCP 保留槽）+ 再 open 抢占槽 0 ----
L=$(wc -l < "$LOG")
send 'writefile /closer.c int syscall3(int n,int a,int b,int c);int main(){syscall3(33,0,0,0);syscall3(30,0,0,0);return 0;}'
wait_for "writefile 写 closer.c" "\[writefile\] '/closer.c' wrote"
send "ccrun /closer.c /closer.elf"
wait_for "F-0b 修复：close id=0 被拒" "\[netsock\] close id=0 DENIED" 20
wait_for "F-0b 修复：closer 运行 PASS" "\[ccrun\] '/closer.elf' exited code=0 PASS" 20

# 修复后：DHCP 保留槽未被关 -> 续约闭环不断流
wait_after "$L" "F-0b 修复：攻击后续约 ACK 仍出现" "\[dhcp\] renew ACK: ip [0-9]\|\[dhcp\] rebind ACK: ip [0-9]" 15
if grep -qa "lease lost -> static fallback" "$LOG"; then
    echo "[FAIL] F-0b 回归：出现 lease lost -> static fallback（DHCP 续约链被打死）"
    FAIL=$((FAIL + 1))
else
    echo "[ok]   F-0b 修复：无 lease lost -> static fallback"
fi

# ---- 观测收口：audit 含 netsock 不变量 + 表满专项日志可见 ----
send "selftest"
wait_for "selftest PASS"        "\[selftest\] PASS (6 checks)" 40
wait_for "audit 含 netsock 健康" "\[audit\] netsock ok: used=" 10
# F-4 撕裂探测器：selftest 汇总行若被内核异步打印（本例 DHCP 续约/RENEW、孤儿 reap）撕裂，
# 会出现"以 [selftest] PASS ( 开头却非整行 checks) 结尾"的残缺行——计数必须为 0。
TEAR=$(grep -aE '^\[selftest\] PASS \(' "$LOG" | grep -avE 'checks\)$' | wc -l)
if [ "$TEAR" -ne 0 ]; then
    echo "[FAIL] F-4 selftest 汇总行被撕裂 ×$TEAR"
    FAIL=$((FAIL + 1))
else
    echo "[ok]   F-4 selftest 汇总行整行无撕裂"
fi

# 稳定后收尾
sleep 1
exec 9>&-
kill "$QPID" 2>/dev/null || true; QPID=""
wait "$CAT_PID" 2>/dev/null || true; CAT_PID=""
rm -f "$TIN" "$TOUT"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "socket 归属回归通过（F-0a 退出回收 / F-0b 保留槽防 close / 观测收口）"
    exit 0
else
    echo "socket 归属回归失败: $FAIL 项未通过"
    exit 1
fi