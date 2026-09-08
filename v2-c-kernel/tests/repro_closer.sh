#!/usr/bin/env bash
# 最小复现：writefile closer.c + ccrun，观察 closer 是否挂死
# 用法: bash tests/repro_closer.sh <kernel.elf> <out.log>
set -u
KERN="$1"; OUT="$2"
TIN=/tmp/rc_in.fifo; TOUT=/tmp/rc_out.fifo
rm -f "$TIN" "$TOUT" "$OUT"
mkfifo "$TIN" "$TOUT"
cat "$TOUT" > "$OUT" & CP=$!
qemu-system-i386 -kernel "$KERN" -display none -vga std -no-reboot -no-shutdown \
  -m 64 -serial stdio -monitor none -netdev user,id=net0 -device e1000,netdev=net0 \
  < "$TIN" > "$TOUT" 2>/dev/null & QP=$!
exec 9>"$TIN"
for i in $(seq 1 40); do grep -aq 'mini-os\$' "$OUT" 2>/dev/null && break; sleep 0.5; done
# 前置步骤（模拟 test_socket.sh F-0a 段）：writefile leak2 + ccrun（开 socket 不关即退出）
printf '%s\n' 'writefile /leak2.c int syscall3(int n,int a,int b,int c);int main(){int i;i=0;while(i<=4){syscall3(30,0,0,0);i=i+1;}return 0;}' >&9
sleep 2
printf '%s\n' 'ccrun /leak2.c /leak2.elf' >&9
sleep 6
printf '%s\n' 'netping' >&9
sleep 3
printf '%s\n' 'writefile /closer.c int syscall3(int n,int a,int b,int c);int main(){syscall3(33,0,0,0);syscall3(30,0,0,0);return 0;}' >&9
sleep 2
printf '%s\n' 'ccrun /closer.c /closer.elf' >&9
sleep 12
exec 9>&-
kill "$QP" "$CP" 2>/dev/null
rm -f "$TIN" "$TOUT"
echo "--- leak2 段 ---"
grep -aE "leak2|ccrun" "$OUT" | tail -4
echo "--- closer 段 ---"
if grep -aq "DENIED" "$OUT" && grep -aq "exited code=0 PASS" "$OUT"; then
  echo "PASS: closer 正常"
elif grep -aq "exited code=0 PASS" "$OUT"; then
  echo "PASS: closer 运行 PASS"
else
  echo "FAIL: closer 挂死"
  grep -aE "fork|wait|elf|exec|ccrun|cc500|DENIED" "$OUT" | tail -10
fi
