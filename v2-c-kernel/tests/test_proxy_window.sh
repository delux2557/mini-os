#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_proxy_window.sh
# 宿主侧**转发器（tcp_proxy.py）窗口/可靠度判据聚合层**——把三个"写好却从没接进 CI"
# 的确定性测试接成一层（零引用已核实：接线前 `grep -rn upstream_window tests Makefile` 空）。
#
# 为什么必须聚合而不是直接接三个：
#   · `test_upstream_reliable.py` / `test_upstream_window.py` 的 proxy 端口是 **argv 入参**
#     （默认 7778），它们**不自拉 proxy** —— 独立直跑必然 `session never opened`（实测 rc=1，733ms）。
#     二者开发时是挂在 `test_tcp.sh` Part A 已启动的代理上跑的，从没有独立入口 ⇒
#     这正是"判据静默失效"的形态：不接门禁就等于不存在。
#   · `test_downlink_window.py` 自拉 proxy（Popen），可独立跑（实测 3.3s PASS）。
#   ⇒ 本层给上行两条**各自拉起独立 proxy 实例**（避免会话状态串味），下行那条沿用自拉。
#
# 断言（防假绿）：
#   1. 三个子判据**全部 rc=0**；任一非 0 → 本层 rc=1（代码病），不吞不退让；
#   2. 每个子判据必须打印自己的 `[PASS]` 行——只有 rc=0 而无标记视为可疑（rc=1）；
#   3. proxy 若在子判据启动前就已退出 = 环境病 → rc=2（不把"环境病"伪装成断言红）；
#   4. 缺 python3 / 所需端口被占用 = 环境病 rc=2（与 test_tcp_attack.sh 同源口径，
#      **不 fuser -k**，绝不偷杀他人进程来"给自己让路"）。
#
# 用法：bash tests/test_proxy_window.sh
#       PROXY_PORT_A/UP_PORT_A/PROXY_PORT_B/UP_PORT_B/DL_PROXY_PORT/DL_UP_PORT 可覆盖默认值。
set -u
cd "$(dirname "$0")/.." || exit 1

for c in python3; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c（环境病）"; exit 2; }
done

# 默认端口刻意避开业务层在用的 7777/7778/8080（`make test` 里同 job 顺序跑，
# 但 CI 的 fast job 与 layer 矩阵共享同一 runner 池，撞上一个被占的口就会得到
# 一条与代码无关的红）。
PA="${PROXY_PORT_A:-7793}"; UA="${UP_PORT_A:-8093}"
PB="${PROXY_PORT_B:-7794}"; UB="${UP_PORT_B:-8094}"
DP="${DL_PROXY_PORT:-7795}"; DU="${DL_UP_PORT:-8095}"

# ---- 所需端口逐个自检；全空闲才继续（占用 = 环境病，早退，别造成 3 连 FAIL） ----
occupied=0
for p in "$PA" "$PB" "$UA" "$UB" "$DP" "$DU"; do
    if python3 - "$p" <<'PY' 2>/dev/null
import socket, sys
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind(('127.0.0.1', int(sys.argv[1])))
except OSError:
    sys.exit(1)          # 被占
finally:
    s.close()
PY
    then :; else echo "[ERR] 环境病：所需端口 $p 已被占用"; occupied=1; fi
done
[ "$occupied" = 1 ] && exit 2

PIDS=""; FAILED=""; RESULT=""
cleanup() { for x in $PIDS; do kill "$x" 2>/dev/null; wait "$x" 2>/dev/null; done; }
trap cleanup EXIT

# start_proxy <tag> <port> —— 拉起该子判据专用的转发器实例
start_proxy() {
    local tag="$1" port="$2"
    python3 tests/tcp_proxy.py --mode udp --port "$port" --timeout 8 \
        >"build-logs/proxy_window_${tag}.log" 2>&1 &
    PIDS="$PIDS $!"; sleep 0.35
    kill -0 "$!" 2>/dev/null || { echo "[ERR] 环境病：proxy 实例(tag=$tag port=$port)启动即退出"; RESULT=env; return 1; }
    return 0
}

mkdir -p build-logs
run_case() { # run_case <名> <脚本> <proxy端口> <上游端口> <是否自拉>
    local name="$1" script="$2" pp="$3" up="$4" selfspawn="$5" rc out
    if [ "$selfspawn" = "no" ]; then
        start_proxy "$name" "$pp" || { FAILED="$FAILED $name(env)"; RESULT=env; return; }
    fi
    out=$(timeout 90 python3 "tests/$script" "$pp" "$up" 2>&1); rc=$?
    printf '  %-22s rc=%-3s %s\n' "$name" "$rc" "$(printf '%s' "$out" | tail -1 | cut -c1-72)"
    # 判据 2：rc=0 还必须给出自己的 PASS 行，否则不认（防"空跑 0 退出"）
    if [ "$rc" = 0 ] && ! printf '%s' "$out" | grep -q '\[PASS\]'; then
        echo "      ↑ 无 [PASS] 标记 ⇒ 视为未真正执行"; FAILED="$FAILED $name(nopass)"; return
    fi
    [ "$rc" = 0 ] || FAILED="$FAILED $name(rc=$rc)"
}

echo "== [proxy-window] 转发器窗口/可靠度宿主判据（上行停等 / 上行滑窗 / 下行滑窗）=="
run_case upstream-reliable test_upstream_reliable.py "$PA" "$UA" no
run_case upstream-window   test_upstream_window.py   "$PB" "$UB" no
run_case downlink-window   test_downlink_window.py   "$DP" "$DU" yes

if [ -n "$FAILED" ]; then
    echo "[FAIL] proxy-window 未通过:$FAILED"
    [ "$RESULT" = env ] && { echo "       （其中含环境病因，请按 exit 2 重跑而非当代码回归）"; exit 2; }
    exit 1
fi
echo "[proxy-window] PASS（3/3 子判据全绿，proxy 侧窗口/乱序/重复 ACK/尾块切分均有断言）"
exit 0
