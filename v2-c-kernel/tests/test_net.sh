#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_net.sh
# v0.18/v0.19/v0.23/v0.25 网络回归：e1000 驱动 + 极简协议栈（ARP + IPv4/UDP + ICMP + DHCP）。
#   1) QEMU `-device e1000` + SLIRP user 网络；宿主起 UDP echo 服务(0.0.0.0:7777)
#   2) 串口日志校验：
#      - 驱动探测 e1000、DHCP DISCOVER/OFFER/REQUEST/ACK 动态取 IP（v0.25）
#      - ARP 请求发出、收到 SLIRP 网关回复（v0.18）
#      - UDP 回环：发 PING -> SLIRP -> 宿主 echo -> PONG 返回（v0.19）
#      - ICMP Echo：发请求到网关 10.0.2.2 -> SLIRP 回显 -> 收到应答（v0.23）
#   3) filter-dump pcap 独立核验：线上确有 ARP 双向交换 与 IPv4/UDP、IPv4/ICMP 包
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh
# v0.33 harness 约定：exit 0=全绿 / 1=断言失败 / 2=环境或依赖缺失（避免环境病伪装成代码病）
for c in qemu-system-i386 socat python3; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

LOG="$BUILD/net.log"
PCAP="$BUILD/net.pcap"
ECHO_LOG="$BUILD/udp_echo.log"
MON="/tmp/minios-net-mon.sock"
FAIL=0
DURATION="${DURATION:-25}"
ECHO_PID=""
RESTORED=0

# v0.28：test_net 用短租期内核（DHCP_RENEW_SECS=2）观察续约闭环；跑完恢复常规内核。
# 短租期下 T1=1s，秒级窗口内可看到 RENEW -> ACK 多轮续约；不影响 ARP/UDP/ICMP 断言
#（续约只是额外 UDP 流量，pcap 计数均为"≥"）。
restore_kernel() {
    [ "$RESTORED" = 1 ] && return
    RESTORED=1
    make clean BUILD="$BUILD" >/dev/null 2>&1 && make BUILD="$BUILD" >/dev/null 2>&1 || true
}

cleanup() {
    [ -n "$ECHO_PID" ] && kill "$ECHO_PID" 2>/dev/null || true
    restore_kernel
    rm -f "$ECHO_LOG"
}
trap cleanup EXIT

echo "== [1/4] 构建内核（DHCP_RENEW_SECS=${DHCP_RENEW_SECS:-2} 短租期：供租期续约回归） =="
make clean BUILD="$BUILD" >/dev/null 2>&1
if ! make DHCP_RENEW_SECS="${DHCP_RENEW_SECS:-2}" BUILD="$BUILD" >/dev/null 2>&1; then
    echo "[FAIL] 内核构建失败"
    exit 1
fi
echo "      构建完成"

echo "== [2/4] 宿主 UDP echo 服务（0.0.0.0:7777） =="
if command -v python3 >/dev/null 2>&1; then
    python3 - > /dev/null 2>&1 <<'EOF' &
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

echo "== [3/4] QEMU 运行 ${DURATION}s（e1000 + SLIRP + pcap 抓包） =="
rm -f "$LOG" "$PCAP" "$MON"
qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none -serial file:"$LOG" \
    -no-reboot -no-shutdown -m 64 \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -object filter-dump,id=fd0,netdev=net0,file="$PCAP" \
    -monitor unix:"$MON",server,nowait >/dev/null 2>&1 &
QPID=$!

# ---- 向 HMP monitor 发一组 sendkey（socat 连接），逐字符转成 sendkey ----
sendkeys() {   # sendkeys <字符串>；空格=spc，换行=ret
    local s="$1" ch cmds=""
    local i
    for ((i = 0; i < ${#s}; i++)); do
        ch="${s:$i:1}"
        case "$ch" in
            ' ')   cmds+="sendkey spc\n" ;;
            $'\n') cmds+="sendkey ret\n" ;;
            *)     cmds+="sendkey $ch\n" ;;
        esac
    done
    printf "%b" "$cmds" | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1
}
wait_after() {   # wait_after <起始行> <说明> <正则> [超时秒]；命中返回 0，超时返回 1
    local start="$1" desc="$2" re="$3" tmo="${4:-8}"
    local i
    for ((i = 0; i < tmo * 4; i++)); do
        if tail -n +$((start + 1)) "$LOG" 2>/dev/null | grep -q "$re"; then
            echo "[ok]   $desc"
            return 0
        fi
        sleep 0.25
    done
    echo "[FAIL] 未等到 $desc (匹配: $re)"
    return 1
}

# 等待自检完成（内核级 UDP 回环 + 用户态 sockdemo 回环均 OK / 任一步失败 / 超时）
END=$((SECONDS + DURATION))
while [ "$SECONDS" -lt "$END" ]; do
    if grep -aq "udp echo: .* -> OK" "$LOG" 2>/dev/null && \
       grep -aq "\[sock\] UDP round-trip OK" "$LOG" 2>/dev/null; then break; fi
    if grep -aq "ARP exchange FAIL\|udp echo FAIL\|icmp echo FAIL\|selftest: tx fail\|UDP round-trip FAIL" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 0.25
done

# ---- v0.22 交互式 netping：等 shell 提示符，注入 `netping`，断言单行 PONG ----
if kill -0 "$QPID" 2>/dev/null; then
    for _ in $(seq 1 40); do
        grep -q 'mini-os\$ ' "$LOG" 2>/dev/null && break
        sleep 0.25
    done
    NET_START=$(wc -l < "$LOG")
    sendkeys $'netping\n'
    wait_after "$NET_START" "shell netping -> PONG" "\[netping\] 10.0.2.2:7777 PONG" 15 || true
fi
# ---- v0.28 等续约闭环（T1=1s）出现再杀 QEMU：DURATION 循环会因 sockdemo 提前 break，
#     早杀会漏掉 tick=100 的首次 RENEW（本轮回归的核心断言）。确定性轮询等待而非固定 sleep。 ----
for _ in $(seq 1 40); do   # 最长 10s
    if grep -aq "\[dhcp\] renew: sent RENEW (unicast)" "$LOG" 2>/dev/null && \
       grep -aq "\[dhcp\] renew ACK: ip [0-9]" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 0.25
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

echo "== [4/4] 校验 =="
check() {   # check "<说明>" "<正则>"
    if grep -aq "$2" "$LOG" 2>/dev/null; then
        echo "[ok]   $1"
    else
        echo "[FAIL] 缺少 $1 (匹配: $2)"
        FAIL=$((FAIL + 1))
    fi
}

check "e1000 探测 + MMIO + 链路"   "\[net\] e1000: MAC .* bar=.* link=1"
# ---- v0.25 DHCP：DISCOVER->OFFER->REQUEST->ACK 动态获取 IP/网关 ----
check "DHCP DISCOVER 发出"         "\[dhcp\] sent DISCOVER"
check "DHCP 收到 OFFER"            "\[dhcp\] OFFER: ip [0-9]"
check "DHCP REQUEST 发出"          "\[dhcp\] sent REQUEST"
check "DHCP 收到 ACK（动态取 IP）" "\[dhcp\] .*ACK: ip [0-9].*gw [0-9]"
# ---- v0.28 DHCP 租期续约：短租期下 T1 单播 RENEW -> ACK，续约闭环（RFC 2131 §4.4.5） ----
check "DHCP 续约 RENEW 发出"     "\[dhcp\] renew: sent RENEW (unicast)"
check "DHCP 续约收到 ACK"        "\[dhcp\] renew ACK: ip [0-9]\|\[dhcp\] rebind ACK: ip [0-9]"
check "ARP 请求发出"               "selftest: tx ARP req (who has 10.0.2.2)"
check "收到 SLIRP ARP 回复"        "selftest: rx ARP reply 10.0.2.2 @ .* -> OK"
check "UDP 发送 PING"              "udp: tx .*B -> 10.0.2.2:7777 (PING)"
check "UDP 回环收到 PONG"          "udp echo: rx .* 'PONG' from .* -> OK"
# ---- v0.23 ICMP Echo：发请求到网关，SLIRP 回显应答（PING 通宿主） ----
check "ICMP 发送 Echo 请求"        "icmp: tx echo req .* -> 10.0.2.2"
check "ICMP 收到 Echo 应答"        "\[icmp\] echo reply from 10.0.2.2 OK"
# ---- v0.20 用户态 UDP socket：sockdemo 经 sys_net_* 系统调用端到端回环 ----
check "sockdemo 进程生成"          "\[boot\] sockdemo pid=[0-9][0-9]*"
check "内核创建 UDP socket"        "\[net\] socket port=0 -> id=[0-9]"
check "用户态 socket 打开"         "\[netsock\] open id=.* port="
check "用户态 sendto PING"         "\[sock\] sendto PING -> 4B"
check "用户态 recvfrom PONG"       "\[sock\] recvfrom PONG +4B from"
check "用户态 UDP 回环 OK"         "\[sock\] UDP round-trip OK"
# ---- v0.22 shell netping 交互命令：单行 PONG + RTT ----
check "shell netping PONG"         "\[netping\] 10.0.2.2:7777 PONG"

# ---- pcap 独立核验：ARP 双向交换 + IPv4/UDP、IPv4/ICMP 包 ----
if [ -s "$PCAP" ] && command -v python3 >/dev/null 2>&1; then
    RES=$(python3 - "$PCAP" <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
endian = '<' if struct.unpack('<I', data[:4])[0] == 0xa1b2c3d4 else '>'
off = 24; req = rep = udp = icmp = 0
while off + 16 <= len(data):
    caplen = struct.unpack(endian + 'I', data[off + 8:off + 12])[0]
    pkt = data[off + 16:off + 16 + caplen]
    if len(pkt) >= 42 and pkt[12:14] == b'\x08\x06':   # ARP
        op = struct.unpack('>H', pkt[20:22])[0]
        if op == 1: req += 1
        elif op == 2: rep += 1
    if len(pkt) >= 42 and pkt[12:14] == b'\x08\x00':    # IPv4
        if pkt[23] == 17: udp += 1                      # UDP
        elif pkt[23] == 1: icmp += 1                    # ICMP
    off += 16 + caplen
print(f"{req} {rep} {udp} {icmp}")
EOF
)
    REQ=$(echo "$RES" | awk '{print $1}')
    REP=$(echo "$RES" | awk '{print $2}')
    UDPN=$(echo "$RES" | awk '{print $3}')
    ICMPN=$(echo "$RES" | awk '{print $4}')
    if [ "${REQ:-0}" -ge 1 ] && [ "${REP:-0}" -ge 1 ]; then
        echo "[ok]   pcap 独立核验: ARP req=$REQ reply=$REP (双向交换成立)"
    else
        echo "[FAIL] pcap 未见完整 ARP 双向交换 (req=$REQ reply=$REP)"
        FAIL=$((FAIL + 1))
    fi
    if [ "${UDPN:-0}" -ge 4 ]; then
        echo "[ok]   pcap 独立核验: IPv4/UDP 包=$UDPN (内核+用户态 PING/PONG 双向)"
    else
        echo "[FAIL] pcap 未见 IPv4/UDP 双向包 (udp=$UDPN)"
        FAIL=$((FAIL + 1))
    fi
    if [ "${ICMPN:-0}" -ge 2 ]; then
        echo "[ok]   pcap 独立核验: IPv4/ICMP 包=$ICMPN (Echo 请求+应答双向)"
    else
        echo "[FAIL] pcap 未见 IPv4/ICMP 双向包 (icmp=$ICMPN)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "[FAIL] 未生成 pcap 或无 python3，无法独立核验线上包"
    FAIL=$((FAIL + 1))
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "网络回归通过（e1000 TX/RX + ARP + IPv4/UDP + ICMP 与宿主端到端互通）"
    exit 0
else
    echo "网络回归失败: $FAIL 项未通过"
    exit 1
fi
