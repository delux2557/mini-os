#!/usr/bin/env bash
# 复刻 test_socket.sh 的完整时序（短租期 DHCP + leak2 + netping + closer + selftest），
# 但 send() 走 ack 背压节拍（等 guest readline 消费确认再发下一条），排除"吞行/合并导致的假冻结"。
# 用法: bash tests/repro_faithful.sh <kernel.elf> <out.log>
set -u
KERN="$1"; OUT="$2"
TIN=/tmp/rf_in.fifo; TOUT=/tmp/rf_out.fifo
# ack 信号：行到达时读方已阻塞 -> [sched] wake keyboard waiter；缓冲已就绪 -> [kb] readline
RP_ACK_RE='\[sched\] wake keyboard waiter pid=[0-9]+ \([0-9]+ bytes\)|\[kb\] readline pid=[0-9]+ -> [0-9]+ bytes'

# 宿主 UDP echo server（netping PONG 依赖，同 test_socket.sh）
python3 - >/dev/null 2>&1 <<'EOF' &
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', 7777))
while True:
    d, a = s.recvfrom(2048)
    s.sendto(b'PONG' + d, a)
EOF
EP=$!
sleep 0.5

rm -f "$TIN" "$TOUT" "$OUT"
mkfifo "$TIN" "$TOUT"
cat "$TOUT" > "$OUT" & CP=$!
qemu-system-i386 -kernel "$KERN" -display none -vga std -no-reboot -no-shutdown \
  -m 64 -serial stdio -monitor unix:/tmp/rf_mon.sock,server,nowait \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  ${QEMU_EXTRA:-} \
  < "$TIN" > "$TOUT" 2>/dev/null & QP=$!
exec 9>"$TIN"
send() {
  local need t
  need=$(grep -acE "$RP_ACK_RE" "$OUT" 2>/dev/null)
  printf '%s\n' "$1" >&9
  # ack 背压：等本行被 guest readline 消费（ack 计数自增）再返回，超时告警但不 fail
  t=0
  while [ "$t" -lt 80 ]; do
    [ "$(grep -acE "$RP_ACK_RE" "$OUT" 2>/dev/null)" -gt "$need" ] && return 0
    sleep 0.25; t=$((t+1))
  done
  echo "  [warn] input ack timeout (${t}s): $1" >&2
  return 1
}
wait_for() { local re="$1" tmo="${2:-25}" i; for ((i=0;i<tmo*4;i++)); do
  grep -aq "$re" "$OUT" 2>/dev/null && return 0; sleep 0.25; done; echo "  [timeout] $re"; return 1; }

wait_for 'mini-os\$ ' 30 || true
wait_for '\[dhcp\] .*ACK: ip [0-9].*gw [0-9]' 30 || true
wait_for '\[dhcp\] renew: sent RENEW (unicast)' 15 || true
wait_for '\[dhcp\] renew ACK: ip [0-9]' 10 || true

send 'writefile /leak2.c int syscall3(int n,int a,int b,int c);int main(){int i;i=0;while(i<=4){syscall3(30,0,0,0);i=i+1;}return 0;}'
wait_for "\[writefile\] '/leak2.c' wrote" 10 || true
send "ccrun /leak2.c /leak2.elf"
wait_for "\[ccrun\] '/leak2.elf' exited code=0 PASS" 20 || true
wait_for "\[netsock\] close id=.* (proc .* exit cleanup)" 10 || true
send "netping"
wait_for "\[netping\] 10.0.2.2:7777 PONG" 15 || true

send 'writefile /closer.c int syscall3(int n,int a,int b,int c);int main(){syscall3(33,0,0,0);syscall3(30,0,0,0);return 0;}'
wait_for "\[writefile\] '/closer.c' wrote" 10 || true
send "ccrun /closer.c /closer.elf"
wait_for "\[netsock\] close id=0 DENIED" 20 || true
wait_for "\[ccrun\] '/closer.elf' exited code=0 PASS" 20 || true

sleep 2
# 冻结现场：抓 QEMU 寄存器（EIP/CR3 判断死循环位置）
if [ -S /tmp/rf_mon.sock ]; then
  { printf 'info registers\ninfo status\nquit\n'; sleep 1; } | socat - unix:/tmp/rf_mon.sock > /tmp/rf_regs.txt 2>/dev/null || true
  echo "--- 冻结时 QEMU 寄存器 ---"
  grep -aE 'EIP=|CR3=|EAX=|running|paused|HALTED|in-kernel|in-KERNEL' /tmp/rf_regs.txt | head -12
fi
exec 9>&-
kill "$QP" "$CP" "$EP" 2>/dev/null
rm -f "$TIN" "$TOUT" /tmp/rf_mon.sock
echo "--- 判定 ---"
if grep -aq "DENIED" "$OUT" && grep -aq "exited code=0 PASS" "$OUT"; then
  echo "PASS: closer 正常"
else
  echo "FAIL: closer 挂死"
  grep -aE "fork|wait pid=3|elf.*loaded|exec|ccrun|cc500|DENIED|\[A\]|\[B\]|netsock|dhcp" "$OUT" | tail -15
fi
