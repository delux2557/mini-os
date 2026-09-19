#!/usr/bin/env bash
# golden.sh — P0-3++ 产物基线：双编译器对固定语料的 产物 sha256 + 运行 exit 值 双列入库。
# v2（#163 教训固化）：sha 锁字节、exit 列锁语义**变化**（#163 的 g03_call 曾以"静默错编译
#   但哈希稳定"的产物入基线两个版本）；语义**正确性**的对照仍由 E 套件/diffsynth 的 gcc 列负责。
#   #160 起语料含 M9c/M9f 形态（g08 子句表/g09 空语句）。
# 用途：任何重构/修复 PR 的"产物面变化"必须显式——golden-check 红了要么回退，
#       要么 `make golden-update` 重生成清单并在 PR 描述声明（改钉协议，与 test-audit 同族）。
# 用法：tests/golden.sh            # 比对（默认）
#       tests/golden.sh --update   # 重生成清单（输出写到 tests/golden/*.sha256）
# 依赖：tests/audit 工具链（build_audit.sh 产物，缺失则自动构建）；
#       编译与**运行**编译器二进制均需 ia32 exec 或 qemu-i386（探测方式与 test_audit.sh 一致）；
#       v2 起逐例实跑产物，故运行步统一 `timeout $RUN_TIMEOUT`——语料里出现死循环/长跑
#       即判红而非挂死 FAST 层（与 verify_findings 的 timeout 10 惯例对齐）。
set -u
K=$(cd "$(dirname "$0")/.." && pwd)             # v2-c-kernel
A=$K/tests/audit; B=$A/bin; G=$K/tests/golden
RUN_TIMEOUT=10
export CC500_SRC=$K/tools/cc500 MINICC_SRC=$K/tools/minicc
# 陈旧产物陷阱（同 test_boundary.sh）：只判"产物存在"会在改了 cc500.c/minicc.c/harness_src 后沿用
# 旧二进制 ⇒ 基线把"代码已改"误报成"产物漂移"。改为按依赖 mtime 判定（--check-stale）。
bash "$A/scripts/build_audit.sh" --check-stale && { bash "$A/scripts/build_audit.sh" >/dev/null || { echo "BUILD FAIL"; exit 2; }; }

RUM=""
( cd "$B" && ./cc500run >/dev/null 2>&1 ); rc=$?
if [ "$rc" = 126 ] && command -v qemu-i386 >/dev/null 2>&1; then
  ( cd "$B" && qemu-i386 ./cc500run >/dev/null 2>&1 ); rc2=$?
  [ "$rc2" != 126 ] && RUM="qemu-i386"
fi
[ "$rc" = 126 ] && [ -z "$RUM" ] && { echo "需要 ia32 exec 或 qemu-i386"; exit 2; }
RUNX(){ if [ -n "$RUM" ]; then "$RUM" "$@"; else "$@"; fi; }
# 运行产物（v2）：timeout 必须**包在 RUM 外层**（RUNX 只能把 RUM 放最前，故另立一支），
# 否则 `qemu-i386 timeout …` 会把宿主 timeout 当 guest 程序去找。
RUNA(){ if [ -n "$RUM" ]; then timeout "$RUN_TIMEOUT" "$RUM" "$@"; else timeout "$RUN_TIMEOUT" "$@"; fi; }

gen(){ # gen <tag> <compiler> <runner> → "hash  exit  case" 行
  # #163 教训落地：sha 锁"字节一致"、exit 列锁"能编≠跑对"的**变化面**（g03_call 曾以错编译入基线两版）。
  local tag=$1 bin=$2 runr=$3 c f ex
  for c in "$G"/cases/*.c; do
    f="g_$tag.elf"
    if ! RUNX "$bin" "$c" "$B/$f" >/dev/null 2>&1; then echo "[$tag] 编译失败: $(basename "$c")"; exit 2; fi
    ( cd "$B" && RUNA "$runr" "$f" >/dev/null 2>&1 ); ex=$?
    # 124/126/127 = "没能跑起来"（超时 / 运行器不可执行），不是真实语义值：
    # 一律判红而非入基线——否则 --update 会把环境病烘进清单，此后 check 反而恒绿。
    case $ex in
      124)     echo "[$tag] 运行超时(${RUN_TIMEOUT}s): $(basename "$c")"; exit 2;;
      126|127) echo "[$tag] 运行器不可执行(rc=$ex): $(basename "$c")"; exit 2;;
    esac
    printf '%s  %s  %s\n' "$(sha256sum "$B/$f" | cut -d' ' -f1)" "$ex" "$(basename "$c")"
  done
}
A1=${1:-}; MODE=${A1#--}; MODE=${MODE:-check}
N=$(ls "$G"/cases/*.c 2>/dev/null | wc -l)
[ "$N" = 0 ] && { echo "[golden] ✘ 语料目录为空：$G/cases/"; exit 2; }
W=$(mktemp -d); trap 'rm -rf "$W" "$B"/g_*.elf' EXIT
for t in cc500 minicc; do
  case $t in cc500) bin="$B/hostcc500"; runr="$B/cc500run";; *) bin="$B/hostminicc32"; runr="$B/runmin32";; esac
  # 三参 gen（编译器+runner）；子壳隔离：gen 内 exit 2 不静默杀主脚本（P2-F 修复保持）
  if ! ( gen "$t" "$bin" "$runr" > "$W/gold_$t" ); then
    { echo "[$t] gen 失败——语料越界/编译不通过（见上；stdout 捕获：)"; cat "$W/gold_$t"; } >&2; exit 2
  fi
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
[ $FAIL = 0 ] && echo "[golden] ✔ 双编译器产物 sha256 + 运行值 与清单逐行一致" || echo "[golden] ✘ 见上"
exit $FAIL
