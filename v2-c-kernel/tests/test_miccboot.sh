#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_miccboot.sh
# V3 自举冒烟：串口驱动 shell 执行 `miccboot`，验证 P1==P2 逐字节一致（自举不动点）。
# 复用 test_serial.sh 的 FIFO 通道模式；专测 minicc 自举链路，断言少而明确。
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh

for c in qemu-system-i386; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

LOG="$BUILD/miccboot.log"
TIN="$BUILD/miccboot_in.fifo"
TOUT="$BUILD/miccboot_out.fifo"
rm -f "$LOG" "$TIN" "$TOUT"
QPID=""; CAT_PID=""

cleanup() {
    exec 9>&- 2>/dev/null || true
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true
    rm -f "$TIN" "$TOUT"
}
trap cleanup EXIT

echo "== [1/3] 构建内核 =="
make BUILD="$BUILD" >/dev/null 2>&1 || { echo "[FAIL] 内核构建失败"; exit 1; }
echo "      构建完成"

echo "== [2/3] QEMU -serial stdio 串口终端 =="
mkfifo "$TIN" "$TOUT"
cat "$TOUT" > "$LOG" & CAT_PID=$!
qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none -vga std \
    -no-reboot -no-shutdown -m 64 -serial stdio -monitor none \
    < "$TIN" > "$TOUT" 2>/dev/null &
QPID=$!
exec 9>"$TIN"

wait_for() {   # wait_for <说明> <正则> [超时秒]
    local desc="$1" re="$2" tmo="${3:-20}" i
    for ((i = 0; i < tmo * 4; i++)); do
        grep -aq "$re" "$LOG" 2>/dev/null && { echo "[ok]   $desc"; return 0; }
        sleep 0.25
    done
    echo "[FAIL] $desc (缺: $re)"
    echo "  >> 现场（LOG 尾 ~30 行）："
    tail -n 30 "$LOG" 2>/dev/null | sed 's/^/      /'
    return 1
}
send() { printf '%s\n' "$1" >&9; sleep 0.3; }

FAIL=0
wait_for "shell 提示符" "mini-os\$ " 30 || FAIL=$((FAIL+1))

echo "== [3/3] miccboot（P1 编译自身 -> P2，P1==P2 校验） =="
SN0=$(wc -l < "$LOG")
send "miccboot"

# 自举编译是大载荷：P1 解析 40KB 源码 + 生成 686KB ELF，TCG 下放宽到 180s
wait_for "minicc-self 被拉起"   "\[elf\] 'minicc-self' loaded" 30
wait_for "P1 编译完成"          "minicc: compiled OK" 180
wait_for "P1 编译退出"          "name=.* code=0" 30
wait_for "自举不动点"           "byte-identical PASS\|P1 != P2 FAIL" 30 \
  || FAIL=$((FAIL+1))

if tail -n "+$((SN0+1))" "$LOG" | grep -aq "P1 != P2 FAIL"; then
    echo "[FAIL] P1 != P2（自举不动点未达成）"; FAIL=$((FAIL+1))
else
    tail -n "+$((SN0+1))" "$LOG" | grep -aq "byte-identical PASS" \
      && echo "[ok]   P1 == P2 逐字节一致" || { echo "[FAIL] 无 PASS 行"; FAIL=$((FAIL+1)); }
fi

sleep 1
exec 9>&-
kill "$QPID" 2>/dev/null || true; QPID=""
wait "$CAT_PID" 2>/dev/null || true; CAT_PID=""
rm -f "$TIN" "$TOUT"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "miccboot 自举冒烟通过（P1 == P2）"
    exit 0
else
    echo "miccboot 自举冒烟失败: $FAIL 项未通过"
    exit 1
fi
