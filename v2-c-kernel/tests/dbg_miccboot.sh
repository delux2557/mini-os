#!/usr/bin/env bash
# 一次性调试：guest 内 miccboot -> 磁盘镜像提取 P1/P2 -> 宿主 cmp/diff
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh
BUILD="$PWD/build"
IMG="$BUILD/miccboot_disk.img"
LOG="$BUILD/miccboot2.log"
TIN="$BUILD/mb2_in.fifo"; TOUT="$BUILD/mb2_out.fifo"
rm -f "$LOG" "$TIN" "$TOUT"
QPID=""; CAT_PID=""

cleanup() { exec 9>&- 2>/dev/null || true; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true; rm -f "$TIN" "$TOUT"; }
trap cleanup EXIT

make BUILD="$BUILD" >/dev/null 2>&1 || { echo "build FAIL"; exit 1; }
echo "build OK"
dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null
echo "disk: $(stat -c%s "$IMG") bytes"

mkfifo "$TIN" "$TOUT"
(cat "$TOUT" > "$LOG") & CAT_PID=$!
qemu-system-i386 -kernel "$BUILD/kernel.elf" -hda "$IMG" -display none -vga std \
    -no-reboot -no-shutdown -m 64 -serial stdio -monitor none \
    < "$TIN" > "$TOUT" 2>/dev/null &
QPID=$!
exec 9>"$TIN"

wait_for() { local d="$1" re="$2" t="${3:-20}" i
  for ((i=0;i<t*4;i++)); do grep -aq "$re" "$LOG" 2>/dev/null && { echo "[ok] $d"; return 0; }; sleep 0.25; done
  echo "[FAIL] $d"; tail -20 "$LOG" | sed 's/^/  /'; return 1; }
send() { printf '%s\n' "$1" >&9; sleep 0.3; }

FAIL=0
wait_for "shell" "mini-os\$ " 30 || FAIL=1
wait_for "ata" "\[ata\] IDE disk:" 10 || FAIL=1
wait_for "format" "disk blank -> format" 10 || FAIL=1
send "miccboot"
wait_for "P1 编译完成" "minicc: compiled OK" 240 || FAIL=1
wait_for "结果行" "byte-identical PASS\|P1 != P2 FAIL\|\[diff\]" 60 || FAIL=1
sleep 1
send "save"
wait_for "save" "\[shell\] save -> 0" 30
sleep 1
exec 9>&-
kill "$QPID" 2>/dev/null || true; QPID=""
wait "$CAT_PID" 2>/dev/null || true; CAT_PID=""
rm -f "$TIN" "$TOUT"

mkdir -p "$BUILD/mb_extract"
python3 tests/extract_fs.py "$IMG" minicc-self out.elf "$BUILD/mb_extract" || true
echo "---- cmp ----"
cmp "$BUILD/mb_extract/minicc-self" "$BUILD/mb_extract/out.elf" && echo "IDENTICAL" || echo "DIFFER"
echo "---- first diffs ----"
cmp -l "$BUILD/mb_extract/minicc-self" "$BUILD/mb_extract/out.elf" 2>/dev/null | head -6
exit $FAIL
