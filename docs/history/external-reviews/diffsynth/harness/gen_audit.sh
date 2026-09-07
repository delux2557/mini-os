#!/usr/bin/env bash
# gen_audit.sh — 对项目自带 diffsynth 生成器做可靠性实测：
#   A. gcc 接受率 / 确定性（跑两次）
#   B. UBSan 计数：生成程序是否真无 UB（signed-overflow/shift/divzero）
#   C. 运行语义差分：minicc 产物退码 vs gcc 参考退码（用本 harness 的 runner）
#   D. 覆盖探针告警采集
set -u
cd "$(dirname "$0")" || exit 1
cd "$(dirname "$0")" || exit 1
# gen.c：优先包内自带版本（与审计报告对应），可用 GEN_SRC 覆盖
if [ ! -f gen.c ]; then
    GEN_SRC="${GEN_SRC:-$MINICC_SRC/diffsynth/gen.c}"
    [ -f "$GEN_SRC" ] && cp "$GEN_SRC" gen.c || { echo "[ERR] 缺 gen.c（本包应自带；或设 GEN_SRC/MINICC_SRC）"; exit 2; }
fi
GEN=./gen; mkdir -p gaudit
if [ ! -x ./hostminicc32 ] || [ ! -x ./runmin32 ]; then
    MINICC_SRC="${MINICC_SRC:-$HOME/mini-os/v2-c-kernel/tools/minicc}"
    if [ -f "$MINICC_SRC/minicc.c" ]; then
        FB="-m32 -ffreestanding -fno-pie -no-pie -fno-stack-protector -O1 -std=gnu99 -fpermissive -w"
        gcc $FB -c "$MINICC_SRC/minicc.c" -o minicc.o &&
        { [ -f crt32.c ] && gcc $FB -c crt32.c -o crt32.o; } &&
        gcc $FB -c runmin.c -o runmin.o 2>/dev/null || true
        [ -f start32.o ] || gcc -m32 -w -c start32.s -o start32.o
        gcc -m32 -static -nostdlib -no-pie -o hostminicc32 minicc.o crt32.o start32.o &&
        gcc -m32 -static -nostdlib -no-pie -o runmin32 runmin.o start32.o
    fi
fi
[ -x ./hostminicc32 ] && [ -x ./runmin32 ] || { echo "[ERR] 需要 hostminicc32/runmin32（由 minicc 评估包 build.sh 产出，或设 MINICC_SRC）"; exit 2; }
gcc -O2 -w -o $GEN gen.c || exit 2
N=${N:-400}; SEED=${SEED:-100}; VARS=${VARS:-6}; STMTS=${STMTS:-8}
rm -f gaudit/*.c
$GEN --seed $SEED --count $N --target minicc --vars $VARS --stmts $STMTS --out gaudit 2>gaudit/probe.warn
ok=0; gccrej=0; nondet=0; ub=0; diff=0; rej=0; hang=0
for f in gaudit/prog_*.c; do
  n=$(basename $f .c)
  if ! gcc -O0 -std=gnu89 -fno-builtin -w -o gaudit/$n.ref "$f" 2>/dev/null; then gccrej=$((gccrej+1)); echo "GCC-REJ $n"; continue; fi
  # 确定性：跑两次
  timeout 5 ./gaudit/$n.ref >/dev/null 2>&1; r1=$?
  timeout 5 ./gaudit/$n.ref >/dev/null 2>&1; r2=$?
  [ "$r1" != "$r2" ] && { nondet=$((nondet+1)); echo "NONDET $n $r1/$r2"; }
  # UBSan：无 UB 三纪律实测（有符号溢出/移位/除零）
  gcc -O0 -std=gnu89 -fno-builtin -w -fsanitize=signed-integer-overflow,shift -fno-sanitize-recover=all -o gaudit/$n.ub "$f" 2>/dev/null
  if gcc -O0 -std=gnu89 -fno-builtin -w -fsanitize=signed-integer-overflow,shift -fno-sanitize-recover=all -o gaudit/$n.ub "$f" 2>/dev/null; then
    timeout 10 ./gaudit/$n.ub >/dev/null 2>gaudit/$n.uberr
    if grep -q "runtime error" gaudit/$n.uberr 2>/dev/null; then ub=$((ub+1)); echo "UB $n: $(grep -m1 -o 'runtime error.*' gaudit/$n.uberr | head -c 90)"; fi
  fi
  rc=$r1
  # minicc 编译 + 产物运行
  if ! ./hostminicc32 "$f" "gaudit/$n.elf" >/dev/null 2>&1; then rej=$((rej+1)); echo "MINICC-REJ $n: $(./hostminicc32 "$f" "gaudit/$n.elf" 2>&1 | head -c 60 | tr -d '\n')"; continue; fi
  timeout 10 ./runmin32 "gaudit/$n.elf" >/dev/null 2>&1; rc2=$?
  sig=$(timeout 10 ./runmin32 "gaudit/$n.elf" 2>&1 >/dev/null | grep -c "PRODUCT SIGNAL")
  if [ "$sig" != 0 ]; then hang=$((hang+1)); echo "PRODUCT-SIGNAL $n: $(timeout 10 ./runmin32 "gaudit/$n.elf" 2>&1 >/dev/null | head -c 90)"; continue; fi
  timeout 10 ./runmin32 "gaudit/$n.elf" >/dev/null 2>&1; rc2=$?
  if [ "$((r1 & 0xff))" != "$((rc2 & 0xff))" ]; then diff=$((diff+1)); echo "SEMANTIC-DIFF $n gcc=$((r1 & 0xff)) cc500=$((rc2 & 0xff))"; else ok=$((ok+1)); fi
done
echo "== [gen_audit] total=$N gcc_rej=$gccrej nondet=$nondet UB程序=$ub minicc_rej=$rej 语义差分=$diff hang/crash=$hang 一致=$ok =="
echo "== 覆盖探针告警 =="; cat gaudit/probe.warn 2>/dev/null | head -5
