#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_tcp_dl.sh
# v1.2 大文件下载回归：虚拟 TCP「边 recv 边累加」下载 128KB（>>TCP_RXB 16KB），验证无总字节上限。
# 用途对照：
#   test_tcp.sh     响应体 8192B (>旧 TCP_RXB)，漏历史 BUG-047
#   test_tcp_dl.sh  响应体 131072B (128KB，8x TCP_RXB)，验证「大文件无上限」——
#                   应用不复用固定 TCP_RXB 缓冲，而是一轮 tcp_recv 拿一块累加。
# 验收：
#   - dldemo 开机 spawn（DL_DEMO=1）
#   - tcp_open/wait_open/tcp_send 成功
#   - recv 累加满 128KB 且 closed=1，尾部 EOFTAIL 完整，RESULT PASS
#   - 独立探针：转发器日志出现 MSG_OPEN / OPENED（wire 双向）
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh
for c in qemu-system-i386 python3 curl; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done
DL_HTTP="${DL_HTTP:-8080}"
DL_UDP="${DL_UDP:-7778}"
FAIL=0
QPID=""; PROXY_PID=""; HTTP_PID=""
RESTORED=0
restore_kernel() {
    [ "$RESTORED" = 1 ] && return; RESTORED=1
    make clean BUILD="$BUILD" >/dev/null 2>&1 && make BUILD="$BUILD" >/dev/null 2>&1 || true
}
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null || true
    [ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null || true
    mkdir -p build-logs 2>/dev/null
    [ -s "$BUILD/tcp_dl.log" ]       && cp "$BUILD/tcp_dl.log"       build-logs/ 2>/dev/null || true
    [ -s "$BUILD/tcp_dl_proxy.log" ] && cp "$BUILD/tcp_dl_proxy.log" build-logs/ 2>/dev/null || true
    restore_kernel
}
trap cleanup EXIT

# 宿主大文件服务：对 /bigup 返回 131072B（128KB）响应体，末尾固定 "EOFTAIL"
run_dl_server() {
    python3 - "$DL_HTTP" <<'PY' &
import socket, sys
port = int(sys.argv[1])
N = 131072
body = b'D' * (N - len(b'EOFTAIL')) + b'EOFTAIL'      # 131072B，末尾 7 字节 EOFTAIL
resp = (b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(body)).encode() +
        b'\r\nConnection: close\r\n\r\n' + body)
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(5)
while True:
    c, _ = s.accept()
    try: c.recv(4096); c.sendall(resp)
    except OSError: pass
    c.close()
PY
    HTTP_PID=$!
    local ok=0 i
    for ((i = 0; i < 15; i++)); do
        if curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$DL_HTTP/" 2>/dev/null | grep -q '200'; then
            ok=1; break
        fi
        sleep 0.4
    done
    if [ "$ok" = 1 ]; then echo "      dl-server pid=$HTTP_PID (self-check 200)"; return 0; fi
    echo "[FAIL] DL HTTP 服务未就绪"; return 1
}

echo "== [1/4] 构建内核（DL_DEMO=1：开机 dldemo 拉 128KB） =="
make clean BUILD="$BUILD" >/dev/null 2>&1
if ! make DL_DEMO=1 BUILD="$BUILD" >/dev/null 2>&1; then echo "[FAIL] Part A 内核构建失败"; exit 1; fi
rm -f "$BUILD/tcp_dl.log" "$BUILD/tcp_dl_proxy.log"
run_dl_server || { echo "[FAIL] 起 DL HTTP 失败"; exit 1; }
python3 tests/tcp_proxy.py --mode udp --port $DL_UDP --log "$BUILD/tcp_dl_proxy.log" >/dev/null 2>&1 &
PROXY_PID=$!
sleep 0.5

echo "== [2/4] QEMU（e1000 + SLIRP）+ 转发器 UDP：dldemo 下载 128KB =="
qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none -m 64 -serial file:"$BUILD/tcp_dl.log" \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QPID=$!
await() {
    local re="$1" tmo="${2:-40}" i
    for ((i = 0; i < tmo * 4; i++)); do
        grep -aq "$re" "$BUILD/tcp_dl.log" 2>/dev/null && return 0
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25
    done
    return 1
}
if ! await "\[http\] DL RESULT " 40; then
    echo "[FAIL] 未等到 dldemo 完成 (DL RESULT)"; FAIL=$((FAIL + 1))
fi
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; QPID=""
kill "$PROXY_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true; PROXY_PID=""
kill "$HTTP_PID" 2>/dev/null || true; wait "$HTTP_PID" 2>/dev/null || true; HTTP_PID=""

echo "== [3/4] dldemo 断言 =="
check() {
    if grep -aq "$2" "$BUILD/tcp_dl.log" 2>/dev/null; then echo "[ok]   $1"; else
        echo "[FAIL] $1 (匹配: $2)"; FAIL=$((FAIL + 1)); fi
}
check "dldemo 开机生成"      "\[boot\] dldemo pid=[0-9][0-9]*"
check "DL tcp_open 成功"     "\[http\] DL tcp_open -> fd="
check "DL tcp_send 成功"     "\[http\] DL tcp_send -> [0-9][0-9]*B"
check "收到 200 OK"          "\[http\] DL HTTP 200 OK"
check "128KB 完整 + closed=1" "\[http\] DL HTTP 200 OK closed=1 len=131072/131072"
check "尾部 EOFTAIL 完整"    "\[http\] DL HTTP 200 OK closed=1 len=131072/131072 tail=EOFTAIL"
check "DL RESULT PASS"       "\[http\] DL RESULT PASS"
if grep -aq "MSG_OPEN sid=" "$BUILD/tcp_dl_proxy.log" 2>/dev/null && \
   grep -aq "OPENED sid=" "$BUILD/tcp_dl_proxy.log" 2>/dev/null; then
    echo "[ok]   转发器独立探针: MSG_OPEN + OPENED"
else
    echo "[FAIL] 转发器未见 MSG_OPEN/OPENED"; FAIL=$((FAIL + 1))
fi

echo "== [4/4] 汇总 =="
if [ "$FAIL" -eq 0 ]; then
    echo "大文件下载回归通过（128KB >>TCP_RXB 16KB，无总字节上限，尾字节完整）"; exit 0
else
    echo "大文件下载回归失败: $FAIL 项未通过"; exit 1
fi