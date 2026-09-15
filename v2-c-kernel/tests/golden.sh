#!/usr/bin/env bash
# golden.sh — P0-3 产物基线清单（audit-gates）：双编译器对固定语料的产物 sha256 入库。
# 用途：任何重构/修复 PR 的"产物面变化"必须显式——golden-check 红了要么回退，
#       要么 `make golden-update` 重生成清单并在 PR 描述声明（改钉协议，与 test-audit 同族）。
# 用法：tests/golden.sh            # 比对（默认）
#       tests/golden.sh --update   # 重生成清单（输出写到 tests/golden/*.sha256）
# 依赖：tests/audit 工具链（build_audit.sh 产物，缺失则自动构建）；
#       运行编译器二进制需 ia32 exec 或 qemu-i386（探测方式与 test_audit.sh 一致，零产物依赖）。
set -u
K=$(cd "$(dirname "$0")/.." && pwd)             # v2-c-kernel
A=$K/tests/audit; B=$A/bin; G=$K/tests/golden
export CC500_SRC=$K/tools/cc500 MINICC_SRC=$K/tools/minicc
[ -x "$B/hostcc500" ] && [ -x "$B/hostminicc32" ] || { bash "$A/scripts/build_audit.sh" >/dev/null || { echo "BUILD FAIL"; exit 2; }; }

RUM=""
( cd "$B" && ./cc500run >/dev/null 2>&1 ); rc=$?
if [ "$rc" = 126 ] && command -v qemu-i386 >/dev/null 2>&1; then
  ( cd "$B" && qemu-i386 ./cc500run >/dev/null 2>&1 ); rc2=$?
  [ "$rc2" != 126 ] && RUM="qemu-i386"
fi
[ "$rc" = 126 ] && [ -z "$RUM" ] && { echo "需要 ia32 exec 或 qemu-i386"; exit 2; }
RUNX(){ if [ -n "$RUM" ]; then "$RUM" "$@"; else "$@"; fi; }

gen(){ # gen <tag> <compiler> → "hash  case" 行
  local tag=$1 bin=$2 c f
  for c in "$G"/cases/*.c; do
    f=$B/g_$tag.elf
    if ! RUNX "$bin" "$c" "$f" >/dev/null 2>&1; then echo "[$tag] 编译失败: $(basename "$c")"; exit 2; fi
    printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(basename "$c")"
  done
}
A1=${1:-}; MODE=${A1#--}; MODE=${MODE:-check}
N=$(ls "$G"/cases/*.c | wc -l)
W=$(mktemp -d); trap 'rm -rf "$W" "$B"/g_*.elf' EXIT
for t in cc500 minicc; do
  case $t in cc500) bin="$B/hostcc500";; *) bin="$B/hostminicc32";; esac
  gen "$t" "$bin" > "$W/gold_$t" || { echo "[$t] gen 内部失败"; exit 2; }
  [ "$(wc -l < "$W/gold_$t")" = "$N" ] || { echo "[$t] 产物行数 $ ≠ 语料 $N —— 有例编译失败"; cat "$W/gold_$t"; exit 2; }
done

if [ "$MODE" = update ]; then
  cp "$W/gold_cc500" "$G/cc500.sha256"; cp "$W/gold_minicc" "$G/minicc.sha256"
  echo "[golden] 清单已重生成（PR 描述须声明：GUARD-CHANGE: golden 重生成 + 原因）"; exit 0
fi
FAIL=0
for t in cc500 minicc; do
  if [ ! -f "$G/$t.sha256" ]; then echo "[golden] 缺清单 $G/$t.sha256（先 make golden-update）"; exit 2; fi
  if diff -u "$G/$t.sha256" "$W/gold_$t" > "$W/$t.diff"; then
    printf '  %-8s golden 一致 (%s 例)\n' "$t" "$(wc -l < "$G/$t.sha256" | tr -d ' ')"
  else
    printf '  %-8s GOLDEN DRIFT ↓（回退，或声明后 make golden-update）\n' "$t"; head -12 "$W/$t.diff" | sed 's/^/    /'; FAIL=1
  fi
done
[ $FAIL = 0 ] && echo "[golden] ✔ 双编译器产物与清单逐哈希一致" || echo "[golden] ✘ 见上"
exit $FAIL
