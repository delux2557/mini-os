#!/usr/bin/env bash
# 快速验证 ccboot 自举不动点（P1==P2）: bash tests/ccboot_check.sh <kernel.elf> <out.log>
set -u
KERN="$1"; OUT="$2"
TIN=/tmp/cb_in.fifo; TOUT=/tmp/cb_out.fifo
rm -f "$TIN" "$TOUT" "$OUT"
mkfifo "$TIN" "$TOUT"
cat "$TOUT" > "$OUT" & CP=$!
qemu-system-i386 -kernel "$KERN" -display none -vga std -no-reboot -no-shutdown \
  -m 64 -nic none -serial stdio -monitor none < "$TIN" > "$TOUT" 2>/dev/null & QP=$!
exec 9>"$TIN"
for i in $(seq 1 60); do grep -aq 'mini-os\$' "$OUT" 2>/dev/null && break; sleep 0.5; done
printf '%s\n' 'ccboot' >&9
# 等结果（最多 180s）
for i in $(seq 1 360); do
  grep -aqE 'byte-identical (PASS|FAIL)|ccboot.*(PASS|FAIL|MISMATCH)' "$OUT" 2>/dev/null && break
  sleep 0.5
done
exec 9>&-
kill "$QP" "$CP" 2>/dev/null
rm -f "$TIN" "$TOUT"
echo "=== ccboot 结果 ==="
grep -aE 'ccboot|byte-identical|compiled OK|PASS|FAIL|MISMATCH' "$OUT" | tail -12
