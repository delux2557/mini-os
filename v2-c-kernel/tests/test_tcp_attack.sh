#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_tcp_attack.sh
# v1.2 压力/攻击回归：虚拟 TCP e1000 通道并行脏注入，验证：
#  1) conn/parse 层脏输入不崩溃：15s 满攻（~600 pkt/s，畸形头/未知 sid/超大 DATA/乱序方向）
#  2) 合法业务（HTTP 8KB+TAIL + closed + refuse）仍完整交付，无副作用
#  3) 攻击源与 guest 真实源独立，tcp_proxy 不会把攻击数据重路由到合法连接
# 依赖：qemu-system-i386 / python3 / curl。端口用环境变量覆盖，避免 CI 冲突。
set -u
cd "$(dirname "$0")/.." || exit 1
for c in qemu-system-i386 python3 curl; do
  command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

HTTP_PORT="${HTTP_PORT:-18080}"
PROXY_UDP="${PROXY_UDP:-18778}"
mkdir -p build-logs
LOG="${LOG:-build-logs/tcp_atk_guest.log}"
PROXY_LOG="${PROXY_LOG:-build-logs/tcp_atk_proxy.log}"
ATK_LOG="${ATK_LOG:-build-logs/tcp_atk_report.log}"
FAIL=0
QPID=""; PROXY_PID=""; HTTP_PID=""; ATK_PID=""

cleanup() {
  [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
  [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null || true
  [ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null || true
  [ -n "$ATK_PID" ] && kill "$ATK_PID" 2>/dev/null || true
  [ -n "$HTTP_PID" ] && wait "$HTTP_PID" 2>/dev/null || true
  [ -n "$PROXY_PID" ] && wait "$PROXY_PID" 2>/dev/null || true
  [ -n "$ATK_PID" ] && wait "$ATK_PID" 2>/dev/null || true
  [ -n "$QPID" ] && wait "$QPID" 2>/dev/null || true
}
trap cleanup EXIT

echo "== [1/5] 构建 TCP_DEMO=1 内核 =="
make clean >/dev/null 2>&1
if ! make TCP_DEMO=1 >/dev/null 2>&1; then
  echo "[FAIL] 内核构建失败"; exit 1
fi
echo "      构建完成"

echo "== [2/5] 宿主 HTTP（8192B+TAIL） + 转发器（UDP:7778，target=8080，匹配 httpdemo 源码硬编码） =="
# 注意：guest 侧 src/app/tcp.c / httpdemo.c 硬编码 TCP_PROXY_PORT=7778 HTTP_PORT=8080 REFUSE_PORT=59998，
# 所以攻击回归也必须用这 3 个端口。
# F3（交接单 处理项）：端口覆盖不贯穿——宿主变量 HTTP_PORT 会被下面吃死为 8080，本回归无法换端口重跑。
# 所需端口若已被占用 = 环境病。不得 fuser -k 误杀他人进程（共享 runner 上会连坐无辜服务），
# 也不得让业务断言静默 5 连 FAIL 伪装成"攻击击穿 TCP 栈"。命中即 exit 2（0/1/2=ok/断言fail/环境病，
# 与仓库统一规范对齐）。
HTTP_PORT=8080; PROXY_UDP=7778; REFUSE_PORT=59998
for pp in "8080/tcp" "7778/udp" "59998/tcp"; do
  pn="${pp%%/*}"
  if ss -ltnu 2>/dev/null | awk '{print $4}' | grep -qE ":${pn}$"; then
    echo "[ERR] 环境病：所需端口 ${pp} 已被占用（guest 编译期硬编码，无法换端口），请释放后重试。"
    exit 2
  fi
done
echo "      运行端口 HTTP=$HTTP_PORT PROXY_UDP=$PROXY_UDP REFUSE=$REFUSE_PORT（与源码硬编码一致）"
python3 - "$HTTP_PORT" <<'PY' &
import socket, sys
port = int(sys.argv[1])
body = b'X' * 8188 + b'TAIL'
resp = (b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(body)).encode() +
        b'\r\nConnection: close\r\n\r\n' + body)
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(5)
while True:
    try:
        c, _ = s.accept()
        try: c.recv(4096); c.sendall(resp)
        except OSError: pass
        c.close()
    except OSError:
        pass
PY
HTTP_PID=$!
# 端口冲突重试：若 bind 报 Address in use，尝试等 + curl 检测
for i in $(seq 1 25); do
  hc=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null || true)
  if [ "$hc" = "200" ]; then break; fi
  if [ "$i" = "25" ]; then echo "[FAIL] 宿主 HTTP 服务未就绪（127.0.0.1:$HTTP_PORT 非200）"; exit 1; fi
  sleep 0.4
done
echo "      http server pid=$HTTP_PID (self-check 200 OK)"

python3 tests/tcp_proxy.py --mode udp --port "$PROXY_UDP" \
  --log "$PROXY_LOG" --idle 20 >/dev/null 2>&1 &
# 说明：MSG_OPEN 载荷里携带目标 HTTP_IP:HTTP_PORT，转发器不需要额外 --target；
# 误传 --target 会 argparse 报错让 proxy 静默退出，guest 永远 0B 回包（务必避免）。
PROXY_PID=$!
for i in $(seq 1 15); do
  if python3 -c "import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.settimeout(0.5)
try:
  s.sendto(b'\x00'*8, ('127.0.0.1', int(sys.argv[1])))
  print('ok')
except Exception:
  print('fail')
" "$PROXY_UDP" 2>/dev/null | grep -q 'ok'; then break; fi
  sleep 0.3
done

echo "== [3/5] 起 QEMU e1000 + 延迟 5s 后并行注入攻击（10s 满攻 1000pkt/s×3 线程） =="
rm -f "$LOG" "$PROXY_LOG" "$ATK_LOG"
: > "$LOG"; : > "$PROXY_LOG"; : > "$ATK_LOG"
qemu-system-i386 -kernel build/kernel.elf -display none -m 64 -serial file:"$LOG" \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -no-reboot -no-shutdown >/dev/null 2>&1 &
QPID=$!
# 攻击器：guest DHCP/session 起步 ~5s 后开攻，duration 10s → 总耗时至少 15s 才出汇总。
# 直接后台挂 bash 子进程，通过 pid 追踪（不要 $(sleep; cmd)&，因为只记录 subshell pid）。
bash -c "sleep 5; exec python3 tests/tcp_attack.py --proxy 127.0.0.1:$PROXY_UDP --duration 10 --rate 300 --threads 3 --wait 3" > "$ATK_LOG" 2>&1 &
ATK_PID=$!
ATK_STARTED=$(date +%s)

echo "      等待 demo 结束（最多 40s）"
for i in $(seq 1 160); do
  if grep -aq "\[http\] RESULT " "$LOG" 2>/dev/null; then break; fi
  if ! kill -0 "$QPID" 2>/dev/null; then break; fi
  sleep 0.25
done
# 等攻击器出汇总：距启动至少 5+10=15s，再给最多 10s 余量
elapsed=$(( $(date +%s) - ATK_STARTED ))
need=$(( 15 - elapsed ))
if [ "$need" -gt 0 ]; then sleep "$need"; fi
for i in $(seq 1 40); do
  kill -0 "$ATK_PID" 2>/dev/null || break
  sleep 0.25
done
# 无论如何先 kill 子进程和 qemu，确保不遗留
kill "$ATK_PID" 2>/dev/null || true; wait "$ATK_PID" 2>/dev/null || true; ATK_PID=""
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; QPID=""
kill "$PROXY_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true; PROXY_PID=""
kill "$HTTP_PID" 2>/dev/null || true; wait "$HTTP_PID" 2>/dev/null || true; HTTP_PID=""

echo "== [4/5] 攻击器汇总 =="
cat "$ATK_LOG" 2>/dev/null || echo "[warn] 无攻击器日志"

echo "== [5/5] 业务侧断言（攻击期间 8KB+TAIL 仍完整交付，不崩不副作用） =="
check() {
  if grep -aq "$2" "$LOG" 2>/dev/null; then
    echo "[ok]   $1"
  else
    echo "[FAIL] $1 (匹配: $2)"; FAIL=$((FAIL + 1))
  fi
}
check "tcp_open 成功"          "\[http\] tcp_open -> fd="
check "HTTP 200 OK"            "\[http\] HTTP 200 OK"
check "closed=1 对端正常关"    "\[http\] HTTP 200 OK closed=1"
check "大响应尾部完整 tail=TAIL" "\[http\] HTTP 200 OK closed=1 len=[1-9][0-9][0-9][0-9][0-9]\? tail=TAIL"
check "拒绝端口 recv=-1"       "\[http\] refuse recv -> -1"
check "RESULT PASS"            "\[http\] RESULT PASS"
# 额外：无内核 panic、无 triple fault（串口日志若有 OOPS / stack overflow 则红）
if grep -Eaq "OOP|stack overflow|PANIC|TRIPLE" "$LOG" 2>/dev/null; then
  echo "[FAIL] 攻击期间内核异常（OOP/stack overflow / PANIC / TRIPLE）"; FAIL=$((FAIL+1))
else
  echo "[ok]   内核无异常标记"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo "[PASS] 虚拟 TCP 压力攻击通过（脏注入 15s，业务侧无副作用）"
  exit 0
else
  echo "[FAIL] $FAIL 项未过"
  echo "--- guest 串口尾部（自诊断） ---"
  tail -n 16 "$LOG" 2>/dev/null | sed 's/^/  serial| /'
  exit 1
fi