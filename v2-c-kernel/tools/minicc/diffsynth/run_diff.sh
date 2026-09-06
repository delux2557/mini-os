#!/usr/bin/env bash
# mini-os/v2-c-kernel/tools/minicc/diffsynth/run_diff.sh
# 差分对拍 harness（宿主 MVP-A）。
#
# 每例生成程序做三重检查：
#   [哨兵] ref_cc=gcc -O0 -m32 能编译 —— 过滤生成器 bug（设计约束 3 有效性门槛）；
#   [确定] gcc 侧跑两次退码一致 + 有限终止 + 无信号 —— 验证无 UB 三纪律（不会挂/崩/除零）；
#   [差分] hostminicc（宿主 gcc 编的 minicc）也能编 —— acceptance 差分：minicc 拒绝
#          gcc 也接受的有效子集程序 = 编译器 bug 候选（子集纪律）。
# ⚠️ 运行语义差分（minicc 产物在 mini-os guest 跑，与 gcc 参考比 stdout/退码）需走 guest，
#    宿主无法直接跑 minicc 产物（产物入口 int $0x80 是 mini-os 契约）——见任务 4 文档，列为下一步。
#
# 用法：
#   bash run_diff.sh --seed 1 --count 20 --target minicc --vars 4 --stmts 6 \
#      [--hostminicc PATH] [--out DIR]
# 退出码：0=全过；1=发现差分/纪律违例；2=环境缺依赖
set -u
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"      # .../v2-c-kernel/tools/minicc/diffsynth
TOOLS_MINI="$(dirname "$SELF_DIR")"             # .../tools/minicc
GEN_SRC="$SELF_DIR/gen.c"
TMPDIR_D="${OUT:-/tmp/diffsynth}_run"
GEN="${GEN:-/tmp/diffsynth/gen}"
HOSTMINICC="${HOSTMINICC:-}"

seed=1; count=10; target=minicc; nv=4; nstmts=6; out="${TMPDIR_D}"
while [ $# -gt 0 ]; do
  key="$1"; val="$2"
  case "$key" in
    --seed)   seed="$val";   shift 2 ;;
    --count)  count="$val";  shift 2 ;;
    --target) target="$val"; shift 2 ;;
    --vars)   nv="$val";     shift 2 ;;
    --stmts)  nstmts="$val"; shift 2 ;;
    --out)    out="$val";    shift 2 ;;
    --gen)    GEN="$val";    shift 2 ;;
    --hostminicc) HOSTMINICC="$val"; shift 2 ;;
    *)        shift ;;
  esac
done
mkdir -p "$out"
mkdir -p "$(dirname "$GEN")"   # P0：GEN 可能落在独立目录（默认 /tmp/diffsynth/gen，out 默认 /tmp/diffsynth_run），须先建其父再 gcc -o

command -v gcc >/dev/null 2>&1 || { echo "[ERR] 缺 gcc"; exit 2; }
command -v qemu-i386 >/dev/null 2>&1 || { echo "[ERR] 缺 qemu-i386"; exit 2; }
[ -x "$GEN" ] || { gcc -O2 -Wall -Wextra -Werror -o "$GEN" "$GEN_SRC" || { echo "[ERR] gen 编译失败"; exit 2; }; }
if [ -z "$HOSTMINICC" ] || [ ! -x "$HOSTMINICC" ]; then
  HOSTMINICC="$out/hostminicc"
  gcc -m32 -std=gnu99 -O1 -w -fpermissive -o "$HOSTMINICC" \
      "$TOOLS_MINI/minicc.c" "$TOOLS_MINI/host_crt.c" || { echo "[ERR] hostminicc 构建失败"; exit 2; }
fi

rm -f "$out"/prog_*.c
rm -f "$out/diffsynth.log"                                   # 失败留档：本 run 违例行追加写此文件（artifact 据此归档现场）
"$GEN" --seed "$seed" --count "$count" --target "$target" --vars "$nv" --stmts "$nstmts" --out "$out" || { echo "[ERR] gen 失败"; exit 2; }

total=0; gcc_reject=0; minic_reject=0; det_fail=0; sig_fail=0; crashtag=0
LOG_D="$out/diffsynth.log"     # 违例留档：tee -a 每次触达；净 stdout（test_diffsynth 看退出码不受扰）
for f in "$out"/prog_*.c; do
  [ -e "$f" ] || continue
  total=$((total+1))
  # 哨兵 + 参考：gcc -O0 -m32 编译（本机无 ia32 exec → 用 qemu-i386 跑）
  exe="${f%.c}.x"
  if ! gcc -O0 -m32 -w -o "$exe" "$f" 2>/dev/null; then
    gcc_reject=$((gcc_reject+1)); echo "[哨兵] gcc 拒 '$f'（生成器 bug，应修 gen）" | tee -a "$LOG_D"; continue
  fi
  # ⚠️ 超时判定不能只看退出码==124：合法的参考程序可能自身 `return 124`
  #（F_CHAR 入网后 char 值域 0..127 可返回 124，PR#110 seed=99 prog_012 即误报"挂起"）。
  # 用 `timeout --verbose`：只有真正触发超时时才往 stderr 打 "sending signal" 标记，
  # 据此与"程序自身退出码 124"可靠区分（coreutils 7.4+）。
  errf="$out/.ref.$$"
  r1=$({ timeout --verbose 5 qemu-i386 "$exe" 2>"$errf"; echo $?; } | tail -1)
  tmo=0; grep -q 'sending signal' "$errf" && tmo=1; rm -f "$errf"
  if [ "$tmo" -eq 1 ]; then sig_fail=$((sig_fail+1)); echo "[纪律] '$f' 超时/挂起 (timeout 触发)" | tee -a "$LOG_D"; continue; fi
  r2=$({ timeout --verbose 5 qemu-i386 "$exe" 2>/dev/null; echo $?; } | tail -1)
  [ "$r1" != "$r2" ] && { det_fail=$((det_fail+1)); echo "[确定] '$f' 两次不一致 $r1/$r2" | tee -a "$LOG_D"; }
  # 差分：minicc 接受否（hostminicc 是 32 位二进制，宿主无 ia32 exec，用 qemu-i386 跑）
  # 设计层区分：fail() 恒 sys_exit(1)=正常拒绝；rc>128=条目下编译器收到信号崩溃（真 bug）；
  #   124=timeout 编译器挂起。三者均视为 acceptance 差分，但分计以利诊断（评审：crash 与拒绝不可分）。
  elf="${f%.c}.elf"
  mout=$({ timeout 20 qemu-i386 "$HOSTMINICC" "$f" "$elf"; } 2>&1); mrc=$?
  if [ $mrc -ne 0 ]; then
    minic_reject=$((minic_reject+1))
    tag="拒绝"
    if   [ $mrc -eq 124 ]; then crashtag=$((crashtag+1)); tag="编译器挂起(timeout=124)";
    elif [ $mrc -gt 128 ]; then crashtag=$((crashtag+1)); tag="编译器崩溃(signum=$((mrc-128)))"; fi
    echo "[差分] minicc $tag '$f'（gcc 接受）：$(echo "$mout" | tail -1)" | tee -a "$LOG_D"
  fi
done

# ---- 违例留档（进 violations/，规避下一批 `rm -f "$out"/prog_*.c` 覆盖）----
# 原缺口：#106 失败留档对 diffsynth 形同虚设——每批开头 rm prog_*.c，seed=99 的违例现场
# 被 seed=2026/cc500 覆盖，artifact 里找不回 prog_012.c。现：本批但凡出现纪律/哨兵/差分违例，
# 即把本批全部 prog + 本批 log 拷入 violations/（该目录不在 rm 范围，跨批可复现追溯）。
if [ "$gcc_reject" -gt 0 ] || [ "$sig_fail" -gt 0 ] || [ "$det_fail" -gt 0 ] || [ "$minic_reject" -gt 0 ]; then
    VIOD="$out/violations/seed-${seed}-${target}"
    mkdir -p "$VIOD"
    cp "$out"/prog_*.c "$VIOD"/ 2>/dev/null
    [ -f "$LOG_D" ] && cp "$LOG_D" "$VIOD/diffsynth.log"
    echo "[留档] 违例样本已拷入 $VIOD/（$total prog + diffsynth.log）" | tee -a "$LOG_D"
fi

echo "== [diffsynth] seed=$seed target=$target total=$total " | tee -a "$LOG_D"
echo "   ref(gcc) 有效=$((total-gcc_reject)) 无效=$gcc_reject  纪律违例(挂/信号)=$sig_fail 确定性错=$det_fail" | tee -a "$LOG_D"
echo "   minicc acceptance 差分：拒绝=$minic_reject（其中编译器崩溃/挂起=$crashtag）" | tee -a "$LOG_D"
if [ "$minic_reject" -eq 0 ] && [ "$sig_fail" -eq 0 ] && [ "$det_fail" -eq 0 ] && [ "$gcc_reject" -eq 0 ]; then
  echo "[diffsynth] PASS" | tee -a "$LOG_D"
  exit 0
else
  echo "[diffsynth] FAIL: minic_reject=$minic_reject sig=$sig_fail det=$det_fail gcc_reject=$gcc_reject" | tee -a "$LOG_D"
  exit 1
fi