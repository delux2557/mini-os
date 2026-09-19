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
# 端口：默认自动挑空闲口（宿主 HTTP + 转发器 UDP），并把 HTTP 口经 DL_PORT 编进 guest；
#       DL_HTTP/DL_UDP 可显式覆盖（会做占用预检）。端口类失败一律 exit 2=环境病。
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh
for c in qemu-system-i386 python3 curl; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done
# ---- 端口：不再硬写 8080/7778 ----------------------------------------
# 旧默认把宿主 HTTP 口钉在 8080：任何占着 8080 的环境（沙箱反向代理、共享 runner、
# 上一轮残留进程）都会让本层红在"起 DL HTTP 失败"上——那是**与代码无关的假红**，
# 也正是这份判据此前从未被接进 CI 的原因之一。现在：
#   · 调用者显式给 DL_HTTP/DL_UDP ⇒ 尊重，但先做占用预检；被占 = 环境病 exit 2；
#   · 未给 ⇒ 自行向后端要一个**空闲口**，并用它编进 guest（见下面 make DL_PORT=…）。
pick_free_port() {   # 让内核挑一个空闲 TCP 端口（bind 0 后读出再关闭）
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(('127.0.0.1', 0))
print(s.getsockname()[1])
s.close()
PY
}
port_free() {        # 探测某显式端口是否可用
    python3 - "$1" "$2" <<'PY'
import socket, sys
fam = socket.AF_INET if sys.argv[2] != 'udp' else socket.AF_INET
kind = socket.SOCK_DGRAM if sys.argv[2] == 'udp' else socket.SOCK_STREAM
s = socket.socket(fam, kind)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind(('127.0.0.1', int(sys.argv[1])))
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
}
# 宿主 HTTP 口：可自动挑空闲口（guest 目标端口由 DL_PORT 编进去，两端同源）。
[ -n "${DL_HTTP:-}" ] || { DL_HTTP=$(pick_free_port) || { echo "[ERR] 环境病：取不到空闲 TCP 端口"; exit 2; }; }
# ⚠ 转发器 UDP 口**不能**自动挑：它是 guest 侧的编译期契约
#   （src/app/tcp.c:21 `#define TCP_PROXY_PORT 7778`，亦见 docs/tcp-session-proto.md 附录 A
#    的线上一跳规定；test_tcp.sh / test_tcp_attack.sh 同用此口）。本测试只做占用预检。
DL_UDP="${DL_UDP:-7778}"
port_free "$DL_HTTP" tcp || { echo "[ERR] 环境病：宿主 HTTP 口 $DL_HTTP 已被占用"; exit 2; }
port_free "$DL_UDP" udp || { echo "[ERR] 环境病：转发器契约口 $DL_UDP(udp) 已被占用（guest 硬编码此口）"; exit 2; }
echo "      端口：宿主 HTTP=$DL_HTTP（自动）/ 转发器 UDP=$DL_UDP（guest 契约口）→ guest 将编入 DL_PORT=$DL_HTTP"
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
    # HTTP 服务侧起不来 = 基础设施/环境病，而不是"下载判据没过"。
    # 早退前先确认子进程还活着：已退出且日志有 bind 错误 ⇒ 端口抢占（环境病）。
    if [ -n "$HTTP_PID" ] && ! kill -0 "$HTTP_PID" 2>/dev/null; then
        echo "[ERR] 环境病：DL HTTP 服务进程已退出（端口被抢或解释器异常）"; return 2
    fi
    echo "[ERR] 环境病：DL HTTP 服务未在超时内就绪（自检 200 未出现）"; return 2
}

echo "== [1/4] 构建内核（DL_DEMO=1：开机 dldemo 拉 128KB） =="
make clean BUILD="$BUILD" >/dev/null 2>&1
# DL_PORT=$DL_HTTP：把 guest 侧要连的目标端口编进 dldemo（Makefile 的 APP_PORT_DEFS），
# 于是"宿主监听口"与"guest 去连的口"由同一个值决定，不再要求各环境都空着 8080。
if ! make DL_DEMO=1 DL_PORT="$DL_HTTP" BUILD="$BUILD" >/dev/null 2>&1; then
    echo "[FAIL] Part A 内核构建失败"; exit 1
fi
rm -f "$BUILD/tcp_dl.log" "$BUILD/tcp_dl_proxy.log"
if ! run_dl_server; then
    rc=$?; [ "$rc" = 2 ] && { echo "[ERR] 起 DL HTTP 失败（环境病，非代码回归）"; exit 2; }
    echo "[FAIL] 起 DL HTTP 失败"; exit 1
fi
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