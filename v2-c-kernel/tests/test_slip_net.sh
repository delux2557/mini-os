#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_slip_net.sh
# 路线图 v1.1 Step 2 端到端回归：COM2 串口网卡（SLIP, RFC 1055）。
#   1) 用 UART_NETIF_DEFAULT 构建（netif 静态绑定串口网卡为当前，D6）
#   2) QEMU：COM1=串口日志（内核输出），COM2=TCP 串口对端（SLIP echo peer）
#   3) 宿主 slip_peer.py 在 COM2 对端做 SLIP 解帧 + UDP/IP 回显（PING->PONG）
#   4) guest 自动 spawn 的 sockdemo 经 netsock->netif(串口) 发 PING / 收 PONG =>
#      `[sock] UDP round-trip OK`，验证"guest UDP 数据报经 COM2 串口往返一致"。
set -u
cd "$(dirname "$0")/.." || exit 1
# v0.33 harness 约定：exit 0=全绿 / 1=断言失败 / 2=环境或依赖缺失
for c in qemu-system-i386 python3; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

LOG="build/slip_net.log"
PEER_LOG="build/slip_peer.log"
SLIP_PORT=7901
FAIL=0
DURATION="${DURATION:-20}"
PEER_PID=""
QPID=""
RESTORED=0

restore_kernel() {
    [ "$RESTORED" = 1 ] && return
    RESTORED=1
    make clean >/dev/null 2>&1 && make >/dev/null 2>&1 || true
}
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$PEER_PID" ] && kill "$PEER_PID" 2>/dev/null || true
    restore_kernel
    rm -f "$LOG" "$PEER_LOG"
}
trap cleanup EXIT

echo "== [1/3] 构建内核（UART_NETIF_DEFAULT=1：netif 静态绑定串口网卡） =="
make clean >/dev/null 2>&1
if ! make UART_NETIF_DEFAULT=1 >/dev/null 2>&1; then
    echo "[FAIL] 内核构建失败"
    exit 1
fi
echo "      构建完成"

echo "== [2/3] QEMU（COM1=日志 / COM2=SLIP 对端）+ 宿主 SLIP 回显对端 =="
rm -f "$LOG" "$PEER_LOG"
qemu-system-i386 -kernel build/kernel.elf -display none -m 64 \
    -serial file:"$LOG" \
    -chardev socket,id=chr2,host=127.0.0.1,port=$SLIP_PORT,server=on,wait=on \
    -serial chardev:chr2 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QPID=$!
python3 tests/slip_peer.py 127.0.0.1 $SLIP_PORT "$PEER_LOG" >/dev/null 2>&1 &
PEER_PID=$!

# 等 SLIP 对端连上 QEMU COM2（guest 的 sockdemo 需在对端就绪后才能收到 PONG）
for _ in $(seq 1 40); do
    grep -q '^CONNECTED$' "$PEER_LOG" 2>/dev/null && break
    sleep 0.25
done

echo "== [3/3] 运行 ${DURATION}s 并校验 =="
END=$((SECONDS + DURATION))
ok=0
while [ "$SECONDS" -lt "$END" ]; do
    if grep -aq "\[sock\] UDP round-trip OK" "$LOG" 2>/dev/null; then ok=1; break; fi
    if grep -aq "UDP round-trip FAIL\|\[uart_netif\] .*FAIL" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 0.25
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

check() {   # check "<说明>" "<正则>"
    if grep -aq "$2" "$LOG" 2>/dev/null; then
        echo "[ok]   $1"
    else
        echo "[FAIL] 缺少 $1 (匹配: $2)"
        FAIL=$((FAIL + 1))
    fi
}
check "COM2 串口网卡初始化（SLIP）"  "\[uart_netif\] COM2 SLIP up"
check "用户态 sendto PING（经串口）" "\[sock\] sendto PING"
check "用户态 recvfrom PONG（经串口）" "\[sock\] recvfrom PONG"
check "用户态 UDP 回环 OK（串口往返）" "\[sock\] UDP round-trip OK"
if grep -aq "PING->PONG" "$PEER_LOG" 2>/dev/null; then
    echo "[ok]   宿主 SLIP 对端收到 PING 并回 PONG"
else
    echo "[FAIL] 宿主 SLIP 对端未完成 PING->PONG 回显"; FAIL=$((FAIL + 1))
fi

echo
if [ "$FAIL" -eq 0 ] && [ "$ok" -eq 1 ]; then
    echo "串口网卡回归通过（guest UDP 数据报经 COM2/SLIP 与宿主对端往返一致）"
    exit 0
elif [ "$FAIL" -eq 0 ] && [ "$ok" -eq 0 ]; then
    echo "[FAIL] guest 未完成串口 UDP 回环"; exit 1
else
    echo "串口网卡回归失败: $FAIL 项未通过"; exit 1
fi