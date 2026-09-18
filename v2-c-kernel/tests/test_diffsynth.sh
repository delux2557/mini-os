#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_diffsynth.sh
# 差分对拍生成器（mini-Csmith）回归。
#
# 断言（防假绿，对齐防假绿纪律）：
#   1. 多 seed × 多目标生成，默认全部 pgm 能被宿主 gcc -O0 -m32 编译（哨兵/有效性门槛）⇒ 生成器无 bug；
#   2. gcc 参考侧两次退码一致且有限终止、无信号 ⇒ 无 UB 三纪律成立（不挂/不除零/不崩）；
#   3. hostminicc 也能编全部 ⇒ acceptance 差分无违例（minicc 不拒绝 gcc 也接受的有效子集程序）；
#   4. cc500 目标额外：cc500 本体接受面 + 产物退码面（装载器 cc500run）与 gcc 参考一致
#      ⇒ CAPS_CC500 的"解禁"有机器证据（P0-6+ 补正；旧网从不调用 cc500）。
#   任一违例 → FAIL（暴露真实的 minicc/cc500/gen 问题，而非假绿）。
# ⚠️ 运行语义差分（minicc 产物在 guest 与 gcc 参考比语义）宿主做不了，见文档任务 4，为下一步。
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh

GEN="$BUILD/diffsynth/gen"
RUN_DIFF=tools/minicc/diffsynth/run_diff.sh
for c in gcc qemu-i386; do
  command -v "$c" >/dev/null 2>&1 || { echo "[SKIP] 缺 $c"; exit 2; }
done

echo "== [1/2] 编译并运行差分 harness（minicc 目标，多 seed） =="
rm -f "$BUILD/diffsynth/gen"   # 强制用最新 gen.c 重建，防陈旧二进制（run_diff 见 -x 才跳）
SEEDS="1 7 42 99 2026"
FAIL=0
for seed in $SEEDS; do
  if ! bash "$RUN_DIFF" --seed "$seed" --count 12 --target minicc --vars 4 --stmts 6 \
       --gen "$GEN" --hostminicc "$BUILD/diffsynth/hostminicc" --out "$BUILD/diffsynth"; then
    echo "[FAIL] minicc 目标 seed=$seed"
    FAIL=1
  fi
done

echo "== [2/2] 动态子集：cc500 目标（能力集裁剪 + cc500 本体三方：gcc × minicc × cc500） =="
# P0-6+（复核补正）：cc500 目标旧网只编 hostminicc —— CAPS_CC500 的"解禁"（F_GLOBAL/F_LEX）
# 没有任何门禁校验（复核实测：cc500 目标 96/96 样本零 cc500 参与）。故此处接入审计工具链，
# 让 cc500 本体既验**接受面**也验**产物退码面**（装载器 cc500run，产物是 mini-os ABI）。
export CC500_SRC="$PWD/tools/cc500" MINICC_SRC="$PWD/tools/minicc"
AB="$PWD/tests/audit/bin"
if [ ! -x "$AB/hostcc500" ] || [ ! -x "$AB/cc500run" ]; then
  bash tests/audit/scripts/build_audit.sh >/dev/null || { echo "[SKIP] 需 tests/audit 工具链（build_audit.sh：gcc-multilib + qemu-i386）"; exit 2; }
fi
for seed in 7 42 99; do
  if ! bash "$RUN_DIFF" --seed "$seed" --count 12 --target cc500 --vars 3 --stmts 5 \
       --gen "$GEN" --hostminicc "$BUILD/diffsynth/hostminicc" \
       --hostcc500 "$AB/hostcc500" --cc500run "$AB/cc500run" --out "$BUILD/diffsynth"; then
    echo "[FAIL] cc500 目标（能力集裁剪）seed=$seed"
    FAIL=1
  fi
done
# F_SUGAR 覆盖断言：cc500 目标样本必须真的采样到语法糖（+= -= /= %= ++ --），
# 否则"解禁"是空洞 PASS——防"已绿但一直绿得错误"（同 GG2 死钉教训）。
SUGAR_N=$(grep -lE '\+=|-=|/=|%=|\+\+|--' "$BUILD"/diffsynth/prog_*.c 2>/dev/null | wc -l)
if [ "$SUGAR_N" -ge 1 ]; then
  echo "[ok]   F_SUGAR 覆盖（$SUGAR_N 个样本含语法糖形态）"
else
  echo "[FAIL] cc500 目标零 F_SUGAR 采样（假绿：解禁未进生成网）"
  FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
  echo "== [diffsynth] PASS =="; exit 0
else
  echo "== [diffsynth] FAIL =="; exit 1
fi