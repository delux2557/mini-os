#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_net.sh
# v0.18 网络回归：e1000 驱动 + ARP 自检（TX+RX 端到端）。
#   1) QEMU `-device e1000` + SLIRP user 网络
#   2) 串口日志校验：驱动探测到 e1000、ARP 请求发出、收到 SLIRP 网关回复
#   3) filter-dump pcap 独立核验：pcap 中确有 ARP req(广播) 与 reply(单播) 双向包
set -u
cd "$(dirname "$0")/.." || exit 1

LOG="build/net.log"
PCAP="build/net.pcap"
FAIL=0
DURATION="${DURATION:-25}"

echo "== [1/3] 构建内核 =="
if ! make >/dev/null 2>&1; then
    echo "[FAIL] 内核构建失败"
    exit 1
fi
echo "      构建完成"

echo "== [2/3] QEMU 运行 ${DURATION}s（e1000 + SLIRP + pcap 抓包） =="
rm -f "$LOG" "$PCAP"
qemu-system-i386 -kernel build/kernel.elf -display none -serial file:"$LOG" \
    -no-reboot -no-shutdown -m 64 \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -object filter-dump,id=fd0,netdev=net0,file="$PCAP" >/dev/null 2>&1 &
QPID=$!

# 等待自检完成（ARP 回复或失败行），最长 DURATION 秒
END=$((SECONDS + DURATION))
DONE=0
while [ "$SECONDS" -lt "$END" ]; do
    if grep -aq "selftest: rx ARP reply" "$LOG" 2>/dev/null; then DONE=1; break; fi
    if grep -aq "ARP exchange FAIL" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 0.25
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

echo "== [3/3] 校验 =="
check() {   # check "<说明>" "<正则>"
    if grep -aq "$2" "$LOG" 2>/dev/null; then
        echo "[ok]   $1"
    else
        echo "[FAIL] 缺少 $1 (匹配: $2)"
        FAIL=$((FAIL + 1))
    fi
}

check "e1000 探测 + MMIO + 链路"   "\[net\] e1000: MAC .* bar=.* link=1"
check "ARP 请求发出"               "selftest: tx ARP req (who has 10.0.2.2)"
check "收到 SLIRP ARP 回复"        "selftest: rx ARP reply 10.0.2.2 @ .* -> OK"

# ---- pcap 独立核验：确认线上确有双向 ARP 交换 ----
if [ -s "$PCAP" ] && command -v python3 >/dev/null 2>&1; then
    RES=$(python3 - "$PCAP" <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
endian = '<' if struct.unpack('<I', data[:4])[0] == 0xa1b2c3d4 else '>'
off = 24; req = rep = 0
while off + 16 <= len(data):
    caplen = struct.unpack(endian + 'I', data[off + 8:off + 12])[0]
    pkt = data[off + 16:off + 16 + caplen]
    if len(pkt) >= 42 and pkt[12:14] == b'\x08\x06':  # ARP (big-endian on wire)
        op = struct.unpack('>H', pkt[20:22])[0]
        if op == 1: req += 1
        elif op == 2: rep += 1
    off += 16 + caplen
print(f"{req} {rep}")
EOF
)
    REQ=$(echo "$RES" | awk '{print $1}')
    REP=$(echo "$RES" | awk '{print $2}')
    if [ "${REQ:-0}" -ge 1 ] && [ "${REP:-0}" -ge 1 ]; then
        echo "[ok]   pcap 独立核验: ARP req=$REQ reply=$REP (双向交换成立)"
    else
        echo "[FAIL] pcap 未见完整 ARP 双向交换 (req=$REQ reply=$REP)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "[FAIL] 未生成 pcap 或无 python3，无法独立核验线上包"
    FAIL=$((FAIL + 1))
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "网络回归通过（e1000 TX/RX + ARP 与 SLIRP 网关端到端互通）"
    exit 0
else
    echo "网络回归失败: $FAIL 项未通过"
    exit 1
fi
