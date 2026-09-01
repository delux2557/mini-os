#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_tcp.sh
# 路线图 v1.1 Step 4 端到端回归：宿主转发器 + 虚拟 TCP 薄包装 HTTP demo。
#   A) e1000 通道：QEMU e1000 + SLIRP，转发器 UDP 通道（127.0.0.1:7778）
#   B) 串口通道：COM2 + SLIP（UART_NETIF_DEFAULT），转发器 SLIP 通道
# 验收点（docs/tcp-thin-api.md §1/§1.1、tcp-mtu-fail.md §2）：
#   - tcp_open -> fd / tcp_send 成功
#   - HTTP 请求-响应拉到 "200 OK"，对端 `Connection: close` 正常关闭 -> recv 返回 0
#   - 连接被拒：转发器回 MSG_ERROR -> recv 返回 -1（与 0 可区分）
#   - 独立探针交叉验证：转发器日志出现 MSG_OPEN / OPENED（复用 Step 2 三方互通信念）
set -u
cd "$(dirname "$0")/.." || exit 1
# v0.33 harness 约定：exit 0=全绿 / 1=断言失败 / 2=环境或依赖缺失
for c in qemu-system-i386 python3; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

HTTP_PORT=8080
PROXY_UDP=7778
SLIP_PORT=7902
FAIL=0
RESTORED=0
QPID=""; PROXY_PID=""; HTTP_PID=""

restore_kernel() {
    [ "$RESTORED" = 1 ] && return
    RESTORED=1
    make clean >/dev/null 2>&1 && make >/dev/null 2>&1 || true
}
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null || true
    [ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null || true
    restore_kernel
    rm -f build/tcp_a.log build/tcp_b.log build/tcp_proxy_a.log build/tcp_proxy_b.log
}
trap cleanup EXIT

run_http_server() {
    python3 -m http.server --bind 127.0.0.1 "$HTTP_PORT" >/dev/null 2>&1 &
    HTTP_PID=$!
    sleep 0.5
}
stop_http() { kill "$HTTP_PID" 2>/dev/null || true; HTTP_PID=""; }

# await_log <log> <label> <regex> <timeout_s>  --- 命中返回0 / 超时返回1
await_log() {
    local log="$1" label="$2" re="$3" tmo="${4:-20}" i
    for ((i = 0; i < tmo * 4; i++)); do
        if grep -aq "$re" "$log" 2>/dev/null; then echo "[ok]   $label"; return 0; fi
        if ! kill -0 "$QPID" 2>/dev/null; then return 1; fi
        sleep 0.25
    done
    echo "[FAIL] 未等到 $label (匹配: $re)"; return 1
}

# ================= Part A：e1000 / UDP 通道 =================
echo "== [A1] 构建内核（TCP_DEMO=1：开机自动 spawn httpdemo） =="
make clean >/dev/null 2>&1
if ! make TCP_DEMO=1 >/dev/null 2>&1; then
    echo "[FAIL] Part A 内核构建失败"; exit 1
fi
rm -f build/tcp_a.log build/tcp_proxy_a.log
run_http_server
python3 tests/tcp_proxy.py --mode udp --port $PROXY_UDP --log build/tcp_proxy_a.log >/dev/null 2>&1 &
PROXY_PID=$!
sleep 0.5
echo "== [A2] QEMU（e1000 + SLIRP）+ 转发器 UDP 通道 =="
qemu-system-i386 -kernel build/kernel.elf -display none -m 64 -serial file:build/tcp_a.log \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QPID=$!
await_log build/tcp_a.log "虚拟 TCP HTTP demo 完成" "\[http\] RESULT " 30 || FAIL=$((FAIL + 1))
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; QPID=""
kill "$PROXY_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true; PROXY_PID=""
stop_http

echo "== [A3] Part A 断言 =="
check() {   # check "<说明>" "<正则>"
    if grep -aq "$2" build/tcp_a.log 2>/dev/null; then
        echo "[ok]   $1"
    else
        echo "[FAIL] 缺少 $1 (匹配: $2)"; FAIL=$((FAIL + 1))
    fi
}
check "httpdemo 开机生成"         "\[boot\] httpdemo pid=[0-9][0-9]*"
check "tcp_open 返回 fd"          "\[http\] tcp_open -> fd="
check "tcp_send 发出 GET"         "\[http\] tcp_send -> [0-9][0-9]*B"
check "收到 HTTP 200 OK"          "\[http\] HTTP 200 OK"
check "对端正常关闭 -> recv 0"    "\[http\] HTTP 200 OK closed=1"
check "连接被拒 -> recv -1（与 0 可区分）" "\[http\] refuse recv -> -1"
check "虚拟 TCP demo 通过"        "\[http\] RESULT PASS"
# 独立探针：转发器日志交叉验证 wire 语义
if grep -aq "MSG_OPEN sid=" build/tcp_proxy_a.log 2>/dev/null && \
   grep -aq "OPENED sid=" build/tcp_proxy_a.log 2>/dev/null; then
    echo "[ok]   转发器日志独立探针: 收到 MSG_OPEN 并回 OPENED（wire 双向成立）"
else
    echo "[FAIL] 转发器日志未见 MSG_OPEN/OPENED（wire 未打通）"; FAIL=$((FAIL + 1))
fi

# ================= Part B：串口（SLIP）通道 =================
echo "== [B1] 构建内核（TCP_DEMO=1 + UART_NETIF_DEFAULT=1：串口网卡） =="
make clean >/dev/null 2>&1
if ! make TCP_DEMO=1 UART_NETIF_DEFAULT=1 >/dev/null 2>&1; then
    echo "[FAIL] Part B 内核构建失败"; exit 1
fi
rm -f build/tcp_b.log build/tcp_proxy_b.log
run_http_server
python3 tests/tcp_proxy.py --mode slip --host 127.0.0.1 --port $SLIP_PORT --log build/tcp_proxy_b.log >/dev/null 2>&1 &
PROXY_PID=$!
sleep 0.5
echo "== [B2] QEMU（COM2 SLIP 通道）+ 转发器 SLIP 通道 =="
qemu-system-i386 -kernel build/kernel.elf -display none -m 64 \
    -serial file:build/tcp_b.log \
    -chardev socket,id=chr2,host=127.0.0.1,port=$SLIP_PORT,server=on,wait=on \
    -serial chardev:chr2 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QPID=$!
await_log build/tcp_b.log "串口通道虚拟 TCP demo 完成" "\[http\] RESULT " 40 || FAIL=$((FAIL + 1))
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; QPID=""
kill "$PROXY_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true; PROXY_PID=""
stop_http

echo "== [B3] Part B 断言 =="
check_b() {
    if grep -aq "$2" build/tcp_b.log 2>/dev/null; then
        echo "[ok]   $1"
    else
        echo "[FAIL] $1 (匹配: $2)"; FAIL=$((FAIL + 1))
    fi
}
check_b "串口通道收到 HTTP 200 OK" "\[http\] HTTP 200 OK"
check_b "串口通道对端正常关闭 -> 0" "\[http\] HTTP 200 OK closed=1"
check_b "串口通道虚拟 TCP demo 通过" "\[http\] RESULT PASS"
if grep -aq "proxy connected to COM2 chardev" build/tcp_proxy_b.log 2>/dev/null; then
    echo "[ok]   转发器已连 COM2（SLIP 通道）"
else
    echo "[FAIL] 转发器未连 COM2"; FAIL=$((FAIL + 1))
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "虚拟 TCP 回归通过（e1000 UDP + 串口 SLIP 双通道，HTTP 200 + 断连 0 + 拒绝 -1）"
    exit 0
else
    echo "虚拟 TCP 回归失败: $FAIL 项未通过"; exit 1
fi