#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_diffsynth.sh
# 差分对拍生成器（mini-Csmith）回归。
#
# 断言（防假绿，对齐防假绿纪律）：
#   1. 多 seed × 多目标生成，默认全部 pgm 能被宿主 gcc -O0 -m32 编译（哨兵/有效性门槛）⇒ 生成器无 bug；
#   2. gcc 参考侧两次退码一致且有限终止、无信号 ⇒ 无 UB 三纪律成立（不挂/不除零/不崩）；
#   3. hostminicc 也能编全部 ⇒ acceptance 差分无违例（minicc 不拒绝 gcc 也接受的有效子集程序）。
#   任一违例 → FAIL（暴露真实的 minicc/gen 问题，而非假绿）。
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

echo "== [2/2] 动态子集：cc500 目标（能力集裁剪，无 for） =="
if ! bash "$RUN_DIFF" --seed 7 --count 10 --target cc500 --vars 3 --stmts 5 \
     --gen "$GEN" --hostminicc "$BUILD/diffsynth/hostminicc" --out "$BUILD/diffsynth"; then
  echo "[FAIL] cc500 目标（能力集裁剪）"
  FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
  echo "== [diffsynth] PASS =="; exit 0
else
  echo "== [diffsynth] FAIL =="; exit 1
fi