#!/usr/bin/env bash
# mini-os/v2-c-kernel/tools/minicc/diffsynth/ddmin.sh
# 薄行级 ddmin（Zeller delta debugging）：把"minicc 误拒"的失败样例缩到最小触发子集。
#
# 宿主可测的差分谓词（acceptance 差分）：被 gcc 接受、却被 minicc(hostminicc) 拒绝的程序
# = minicc 误拒有效子集程序的 bug 候选。ddmin 通过逐行删除，找出**仍触发误拒的最小行子集**，
# 定位 bug 时不必看整份长样例。
#
# 用法：
#   bash ddmin.sh <src.c>                 # 用默认谓词（需宿主 gcc + hostminicc 可建）
#   DD_PROG=<path> bash ddmin.sh <src.c>  # 自定义"坏"判定：DD_PROG 打印/退出 0=坏(仍触发)
#   env DD_BUILD=<dir>                     # hostminicc 构建目录（缓存）
# 输出：最小复现写到 <src>.min.c；stdout 打印行数缩减。
set -u
SRC="${1:?usage: ddmin.sh <src.c>}"
DIR="$(dirname "$(readlink -f "$SRC")")"
BUILD_D="${DD_BUILD:-/tmp/ddmin_build}"
mkdir -p "$BUILD_D"

# 宿主可测谓词（默认）：gcc -O0 接受 且 hostminicc 拒绝 ⇒ 坏(触发)；否则好(不触发，可删)
command -v gcc >/dev/null 2>&1 || { echo "[ERR] 缺 gcc"; exit 2; }
command -v qemu-i386 >/dev/null 2>&1 || { echo "[ERR] 缺 qemu-i386"; exit 2; }
HM="$BUILD_D/hostminicc"
if [ ! -x "$HM" ]; then
  SELF="$(cd "$(dirname "$0")" && pwd)"
  TOOLS_MINI="$(dirname "$SELF")"
  gcc -m32 -std=gnu99 -O1 -w -fpermissive -o "$HM" \
      "$TOOLS_MINI/minicc.c" "$TOOLS_MINI/host_crt.c" || { echo "[ERR] hostminicc 构建失败"; exit 2; }
fi

is_bad() {   # is_bad <cand>  → 0 坏(仍触发)，1 好
  local f="$1"
  if [ -n "${DD_PROG:-}" ]; then
    [ "$("$DD_PROG" "$f" 2>/dev/null)" = 0 ]; return
  fi
  # 默认谓词
  if ! gcc -O0 -m32 -w -o "$BUILD_D/gcc.deleteme" "$f" 2>/dev/null; then return 1; fi  # gcc 拒=无效，视"好"不采信
  if qemu-i386 "$HM" "$f" "$BUILD_D/out.elf" >/dev/null 2>&1; then return 1; fi      # minicc 接受=好
  return 0                                                                           # gcc 接受 & minicc 拒=坏
}

# 原始是否为坏
if ! is_bad "$SRC"; then echo "[info] 原始样例未触发（无 bug；ddmin 无米之炊）"; exit 0; fi

# 备份原始行
cp "$SRC" "$SRC.orig.c"
mapfile -t LINES < "$SRC"

# ---- 经典 ddmin（Zeller）：粒度从 n 向下折半，同一粒度反复删除到不能再删 ----
mapfile -t LINES < "$SRC"; n=${#LINES[@]}
granul=$n
while [ "$granul" -ge 1 ]; do
  : > /dev/null
  while :; do
    removed=0
    start=0
    while [ $start -lt $n ]; do
      end=$(( start + granul )); [ $end -gt $n ] && end=$n
      # 候选 = 去掉 [start,end)
      : > "$SRC.min.c"
      for ((k=0;k<start;k++)); do printf '%s\n' "${LINES[k]}" >> "$SRC.min.c"; done
      for ((k=end;k<n;k++)); do printf '%s\n' "${LINES[k]}" >> "$SRC.min.c"; done
      if is_bad "$SRC.min.c"; then
        mapfile -t LINES < "$SRC.min.c"; n=${#LINES[@]}
        start=0; removed=1
      else
        start=$end
      fi
    done
    [ "$removed" -eq 0 ] && break
  done
  granul=$(( granul / 2 ))
done

: > "$SRC.min.c"
for ((k=0;k<n;k++)); do printf '%s\n' "${LINES[k]}" >> "$SRC.min.c"; done
printf '[ddmin] %s: %d 行 → %d 行（最小触发子集见 %s.min.c）\n' "$SRC" "$(wc -l < "$SRC.orig.c")" "$n" "$SRC"
exit 0