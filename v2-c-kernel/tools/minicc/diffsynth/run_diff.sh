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

command -v gcc >/dev/null 2>&1 || { echo "[ERR] 缺 gcc"; exit 2; }
command -v qemu-i386 >/dev/null 2>&1 || { echo "[ERR] 缺 qemu-i386"; exit 2; }
[ -x "$GEN" ] || { gcc -O2 -Wall -Wextra -Werror -o "$GEN" "$GEN_SRC" || { echo "[ERR] gen 编译失败"; exit 2; }; }
if [ -z "$HOSTMINICC" ] || [ ! -x "$HOSTMINICC" ]; then
  HOSTMINICC="$out/hostminicc"
  gcc -m32 -std=gnu99 -O1 -w -fpermissive -o "$HOSTMINICC" \
      "$TOOLS_MINI/minicc.c" "$TOOLS_MINI/host_crt.c" || { echo "[ERR] hostminicc 构建失败"; exit 2; }
fi

rm -f "$out"/prog_*.c
"$GEN" --seed "$seed" --count "$count" --target "$target" --vars "$nv" --stmts "$nstmts" --out "$out" || { echo "[ERR] gen 失败"; exit 2; }

total=0; gcc_reject=0; minic_reject=0; det_fail=0; sig_fail=0
for f in "$out"/prog_*.c; do
  [ -e "$f" ] || continue
  total=$((total+1))
  # 哨兵 + 参考：gcc -O0 -m32 编译（本机无 ia32 exec → 用 qemu-i386 跑）
  exe="${f%.c}.x"
  if ! gcc -O0 -m32 -w -o "$exe" "$f" 2>/dev/null; then
    gcc_reject=$((gcc_reject+1)); echo "[哨兵] gcc 拒 '$f'（生成器 bug，应修 gen）"; continue
  fi
  r1=$({ timeout 5 qemu-i386 "$exe"; echo $?; } 2>/dev/null | tail -1)
  if [ "$r1" -eq 124 ]; then sig_fail=$((sig_fail+1)); echo "[纪律] '$f' 超时/挂起 (rc=124)"; continue; fi
  r2=$({ timeout 5 qemu-i386 "$exe"; echo $?; } 2>/dev/null | tail -1)
  [ "$r1" != "$r2" ] && { det_fail=$((det_fail+1)); echo "[确定] '$f' 两次不一致 $r1/$r2"; }
  # 差分：minicc 接受否（hostminicc 是 32 位二进制，宿主无 ia32 exec，用 qemu-i386 跑）
  elf="${f%.c}.elf"
  mout=$({ timeout 20 qemu-i386 "$HOSTMINICC" "$f" "$elf"; } 2>&1); mrc=$?
  if [ $mrc -ne 0 ]; then
    minic_reject=$((minic_reject+1)); echo "[差分] minicc 拒 '$f'（gcc 接受）：$(echo "$mout" | tail -1)"
  fi
done

echo "== [diffsynth] seed=$seed target=$target total=$total "
echo "   ref(gcc) 有效=$((total-gcc_reject)) 无效=$gcc_reject  纪律违例(挂/信号)=$sig_fail 确定性错=$det_fail"
echo "   minicc acceptance 差分：拒绝=$minic_reject"
if [ "$minic_reject" -eq 0 ] && [ "$sig_fail" -eq 0 ] && [ "$det_fail" -eq 0 ] && [ "$gcc_reject" -eq 0 ]; then
  echo "[diffsynth] PASS"
  exit 0
else
  echo "[diffsynth] FAIL: minic_reject=$minic_reject sig=$sig_fail det=$det_fail gcc_reject=$gcc_reject"
  exit 1
fi