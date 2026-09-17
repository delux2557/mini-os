#!/usr/bin/env bash
# test_boundary.sh — roadmap:193 点名"边界程序集 cc/minicc vs gcc 差分进 CI"的落成（FAST 层）。
# 每形态三方各编各跑：**期望表即语言契约台账**——
#   OK 行：三方 rc 必须逐位一致（任一侧漂离 = 回归）；
#   DIV 行：**显式登记的已知分歧**（cc500 无类型面 char 宽）——期望仍逐位锁死，
#       分歧"变大/变小/消失"都要红（消失也许意味着该修 ledger 了，那也是红给你看）。
#   SPLIT-M / SPLIT-C：minicc/cc500 一方能力缺口（goto/switch 家族——minicc 待补，
#       见 #170/#171 台账）；同样钉死"缺口形状"，静默扩权也红。
# 值域全部按 exit 低 8 位比较（三方同构口径）；编译期消息文本不断言（只 REJ 档位）。
set -u
K=$(cd "$(dirname "$0")/.." && pwd)
A=$K/tests/audit; B=$A/bin
export CC500_SRC=$K/tools/cc500 MINICC_SRC=$K/tools/minicc
[ -x "$B/hostcc500" ] || bash "$A/scripts/build_audit.sh" >/dev/null || { echo "[boundary] BUILD FAIL"; exit 2; }
( cd "$B" && ./cc500run >/dev/null 2>&1 ); rc=$?
RUM=""
if [ "$rc" = 126 ] && command -v qemu-i386 >/dev/null 2>&1; then RUM="qemu-i386"; fi
RUNX(){ if [ -n "$RUM" ]; then "$RUM" "$@"; else "$@"; fi; }
RUMX(){ if [ -n "$RUM" ]; then echo "$RUM"; else echo ""; fi;}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
PASS=0; FAILN=0
say(){ printf '  %-22s %s %s\n' "$1" "$2" "$3"; }
# 运行一档编译器：rc 归一为 GCC 语义的数值档
probe(){ # probe <bin> <run> <srcfile> — echo V<n>|REJ|SIG
  local cb=$1 rb=$2 src=$3 r
  RUNX "$cb" "$src" "$W/o.elf" >/dev/null 2>&1 || { echo REJ; return; }
  if [ -n "$RUM" ]; then ( cd "$W" && timeout 5 "$RUM" "$rb" o.elf >/dev/null 2>&1 ); else ( cd "$W" && timeout 5 "$rb" o.elf >/dev/null 2>&1 ); fi; r=$?
  # 统一口径：值=rc&255；信号/异常（rc≥128，含 timeout 124/137）→ 哨兵 165（各侧同构）
  if [ $r -ge 128 ]; then echo 165; else echo $r; fi
}
chk(){ # chk <名> <gcc期望> <cc期望> <min期望> <源码>
  local name=$1 eg=$2 ec=$3 em=$4 src=$5 g c m verdict
  printf '%s' "$src" > "$W/t.c"
  rm -f "$W/gx"
  if gcc -O0 -w -fno-builtin -o "$W/gx" "$W/t.c" 2>/dev/null; then
    ( cd "$W" && timeout 5 "$W/gx" >/dev/null 2>&1 ); local gr=$?
    [ $gr -ge 128 ] && g=165 || g=$((gr & 255))
  else g=REJ; fi
  c=$(probe "$B/hostcc500" "$B/cc500run" "$W/t.c")
  m=$(probe "$B/hostminicc32" "$B/runmin32" "$W/t.c")
  if [ "$g" = "$eg" ] && [ "$c" = "$ec" ] && [ "$m" = "$em" ]; then
    PASS=$((PASS+1)); say "$name" "=$eg/$ec/$em" "PASS"
  else
    FAILN=$((FAILN+1)); say "$name" "gcc=$g cc=$c min=$m" "FAIL(期望 $eg/$ec/$em)"
  fi
}
echo "== [boundary] 双编译器×gcc 语言契约三方台账（FAST）=="
# ── 全拒家族（数制/字面量纪律，MC-07/E5/M9）──
# 注：`010` 是合法 C 八进制（gcc=V8）；两侧显式拒=子集纪律，gcc 列锁真实值防误导
chk octal            8    REJ REJ 'int main(){int a;a=010;return a;}'
chk emptyhex-prim    REJ REJ REJ 'int main(){int a;a=0x;return a;}'
chk SPLIT-M-hexcase  REJ REJ REJ 'int main(){int v;v=1;switch(v){case 0x:return 1;}return 0;}'
# 注：gcc 对未知串转义按字面处理（'a\qb'→q），两侧子集纪律拒——gcc 更宽族台账行
chk SPLIT-G-unkesc   0   REJ REJ 'int main(){char *s;s="a\qb";return 0;}'
chk unkesc-char      122 REJ REJ 'int main(){char c;c='\''\z'\'';return c;}'
chk unclosed-str     REJ REJ REJ 'int main(){char *s;s="ab;return 0;}'
chk unclosed-cmt     REJ REJ REJ 'int main(){return /*'
chk redef-fn         REJ REJ REJ 'int f(){return 1;}int f(){return 2;}int main(){return f();}'
chk uninit-goto-fwd  REJ REJ REJ 'int main(){goto nod;return 0;}'
chk dupcase          REJ REJ REJ 'int main(){int v;v=1;switch(v){case 1:return 1;case 1:return 2;}return 0;}'
chk amp-nonlval      REJ REJ REJ 'int main(){return &3;}'
chk SPLIT-C-fordecl  0   REJ REJ 'int main(){for(int i=0;i<2;i=i+1);return 0;}'
# ── 全接受家族（对称正例，逐位同值）──
chk deref-char       0   0   0   'int main(){char v;char *p;v=65;p=&v;return *p-65;}'
chk idx-char         0   0   0   'int main(){char v;char *p;v=66;p=&v;return p[0]-66;}'
chk str-deref-k      0   0   0   'int main(){char *s;s="abc";return *(s+1)-98;}'
chk mul-chain        0   0   0   'int main(){int a,b;a=3;b=4;return a*b-12;}'
chk loop-logic       0   0   0   'int main(){int i,s;i=0;s=0;while(i<5){if(i%2){i=i+1;continue;}s=s+i;i=i+1;}return s-6;}'
chk call-rec         0   0   0   'int f(int n){if(n<=0){return 0;}return n+f(n-1);}int main(){return f(4)-10;}'
# ── 显式登记分歧：cc500 无类型面，`*p` 恒 char 宽（#171 契约的镜像钉；值 crafted 分离）──
chk DIV-int-deref-w  34   32  34   'int main(){int v;int *p;v=0x0102;p=&v;return (*p*67+3)%35;}'
# ── 能力缺口台账（非分歧，是"minicc 待补 goto/switch 家族"的形状快照，见 #165/#171 后续）──
chk SPLIT-M-goto     0   0   REJ 'int main(){goto e;return 9;e:return 0;}'
chk SPLIT-M-switch   1   1   REJ 'int main(){int v;v=1;switch(v){case 1:return 1;}return 0;}'
chk SPLIT-M-fall     0   0   REJ 'int main(){int v,r;r=0;v=1;switch(v){case 1:r=r+1;case 2:r=r+10;break;default:r=9;}return r-11;}'
# ── cc500 接受面缺口（arity 不校验：静默产出按既有参数个数跑 → 与"拒"的期望都锁死）──
chk SPLIT-C-arity    REJ 125 REJ 'int f(int a,int b){return a+b;}int main(){int x;x=f(1);return x;}'
chk SPLIT-C-starnum  REJ 165 REJ 'int main(){return *3;}'
echo "----"
if [ $FAILN = 0 ]; then echo "[boundary] PASS($PASS 形态·三方逐位) "; exit 0; fi
echo "[boundary] FAIL: $FAILN 漂移（详见 ↑）"; exit 1
