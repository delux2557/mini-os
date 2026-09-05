#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_diffsynth_guest.sh
# V4 mini-Csmith 差分对拍【guest 运行语义差分】（方案 B：.elf 批量嵌入，一次 QEMU）。
#
# 思路：默认构建零侵入；本脚本用独立语义以 `GUEST_DIFF=1` 构建一个嵌有 12 个随机样本(ds00..ds11)
# 的内核，QEMU 启动后在 guest 里逐个 `run dsXX` 拿**运行语义退码**，与宿主 gcc 参考差分比对。
# 这是宿主 acceptance 差分(MVP-A / test_diffsynth.sh)的补全——minicc 产物 innite mini-os 契约，
# 只能进 guest 跑。
#
# 比对口径：程序 `return EXPR`，gcc 侧退码= qemu-i386 跑 (return&0xff)；guest `run dsXX` 的
# `[shell] 'dsXX' exited code=<n>` 同理。二者相等即 PASS（防假绿：比语义可比对单一判据）。
# 退出码：0=全绿 / 1=差分失败 / 2=环境缺依赖或构建失败
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh

for c in qemu-system-i386 qemu-i386 gcc; do
  command -v "$c" >/dev/null 2>&1 || { echo "[SKIP] 缺 $c"; exit 2; }
done

# 用独立构建子目录（即便 BUILD=build 也只写 build/guest_diff_build/，绝不覆盖共享 build/kernel.elf）
GB="$BUILD/guest_diff_build"
DS_DIR="$GB/diffsynth"
NAMES="ds00 ds01 ds02 ds03 ds04 ds05 ds06 ds07 ds08 ds09 ds10 ds11"

echo "== [1/3] 构建 GUEST_DIFF 内核（嵌随机样本 initramfs） =="
if ! make GUEST_DIFF=1 BUILD="$GB"; then
  echo "[ERR] GUEST_DIFF 内核构建失败"; exit 2
fi
echo "[ok] 内核构建完成"

echo "== [2/3] 宿主 gcc 参考退码（每个 dsXX__src.c） =="
declare -A REF
for n in $NAMES; do
  src="$DS_DIR/${n}__src.c"; exe="$DS_DIR/${n}.gccx"
  gcc -O0 -m32 -w -o "$exe" "$src" 2>/dev/null || { echo "[SKIP] gcc 拒 $src"; exit 2; }
  REF[$n]=$(timeout 5 qemu-i386 "$exe"; echo $?)
  echo "[ok] $n gcc-ref=${REF[$n]}"
done

echo "== [3/3] QEMU guest 运行语义差分（run dsXX 比对退码） =="
LOG="$GB/guest_diff.log"; TIN="$GB/gd_in.fifo"; TOUT="$GB/gd_out.fifo"
rm -f "$LOG" "$TIN" "$TOUT"; mkfifo "$TIN" "$TOUT"
cat "$TOUT" > "$LOG" & CATPID=$!
qemu-system-i386 -kernel "$GB/kernel.elf" -display none -vga std \
  -no-reboot -no-shutdown -m 64 -serial stdio -monitor none \
  < "$TIN" > "$TOUT" 2>/dev/null &
QPID=$!; exec 9>"$TIN"

send() { printf '%s\n' "$1" >&9; sleep 0.3; }
wait_for() { local re="$1" tmo="${2:-20}" i; for ((i=0;i<tmo*4;i++)); do
  grep -aq "$re" "$LOG" 2>/dev/null && return 0; sleep 0.25; done; return 1; }

wait_for 'mini-os\$ ' 30 || { echo "[ERR] guest 未起 shell"; kill "$QPID" 2>/dev/null; exit 1; }

FAIL=0
for n in $NAMES; do
  ref=${REF[$n]}
  send "run $n"
  # 解析 guest 打印的退出码：mini-os exit_code 是完整 uint32（负 return 显示巨大数），
  # 与 Linux/qemu 的 &0xff 编码不同 → 统一按 return 值低 8 位语义对齐比对。
  gline=""
  for ((i=0;i<60;i++)); do
    gline=$(grep -aoE "\[shell\] '$n' exited code=[0-9]+" "$LOG" 2>/dev/null | tail -1)
    [ -n "$gline" ] && break
    sleep 0.25
  done
  if [ -z "$gline" ]; then
    if grep -aq "cannot load '$n'" "$LOG"; then echo "[DIFF] $n minicc 未嵌入/拒绝 (gcc-ref=$ref)"; else echo "[DIFF] $n 无退出行（时序） (gcc-ref=$ref)"; tail -3 "$LOG" | sed 's/^/    | /'; fi
    FAIL=1; continue
  fi
  gcode="${gline##*=}"
  allow=$(( gcode & 255 ))
  if [ "$allow" -eq "$ref" ]; then
    echo "[PASS] $n guest-code=$gcode (&0xff=$allow)==gcc-ref=$ref"
  else
    echo "[DIFF] $n guest-code=$gcode (&0xff=$allow) != gcc-ref=$ref"
    FAIL=1
  fi
done

kill "$QPID" 2>/dev/null; kill "$CATPID" 2>/dev/null
exec 9>&-
rm -f "$TIN" "$TOUT"

if [ "$FAIL" -eq 0 ]; then echo "== [diffsynth-guest] PASS (12/12 语义一致) =="; exit 0; else echo "== [diffsynth-guest] FAIL =="; exit 1; fi