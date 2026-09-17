#!/usr/bin/env bash
# test_boundary.sh — roadmap:193 点名"边界程序集 cc/minicc vs gcc 差分进 CI"的落成（FAST 层）。
# 每形态三方各编各跑：**期望表即语言契约台账**。**行的前缀就是台账分类，勿错标**——未来
# minicc 补齐能力时，"翻正哪些行"完全依赖前缀检索，错标会直接污染那条通道：
#   （无前缀）全拒族：三方都必须 REJ（子集纪律；gcc 列如为真实值则另见 SPLIT-G）；
#   SPLIT-G：gcc 更宽（gcc 接受/有值，cc500 与 minicc 均拒 = 有意子集边界）；
#   SPLIT-M：minicc 能力缺口（goto/switch 家族 —— minicc 待补，见 #170/#171 台账）；
#   SPLIT-C：cc500 接受面缺口（arity 不校验、`*3` 不阻编译 ⇒ 编得过但运行期自成后果）；
#   DIV：**显式登记的已知分歧**（cc500 无类型面 ⇒ deref 恒 char 宽；#171 契约）——期望仍逐位
#       锁死，"分歧变大/变小/消失"都要红（消失也许意味着该修本台账了，那也是红给你看）。
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
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
PASS=0; FAILN=0
say(){ printf '  %-22s %s %s\n' "$1" "$2" "$3"; }
# 运行产物并**归一退出码**（三方同构口径）：值 = rc&255；信号/异常（rc≥128，含 timeout 124/137）→ 哨兵 165。
# ⚠ 必须用**内层 bash** 裹住整条命令：bash 对被信号杀死的子进程会往自身 stderr 打一行
#   `Segmentation fault` —— 而"崩溃"在本台账里是**期望内**行为（SPLIT-C-starnum 就是要它崩），
#   不该污染 FAST 层日志。连内层 bash 一起重定向即可：内层 bash 会以 139 正常退出，仍 ≥128 ⇒ 仍归一到 165。
runc(){ # runc <runner> — 在 $W 下跑 o.elf（必要时经 RUM），echo 归一档
  local r
  bash -c "cd \"$W\" && timeout 5 ${RUM:+$RUM }\"$1\" o.elf >/dev/null 2>&1" 2>/dev/null; r=$?
  if [ $r -ge 128 ]; then echo 165; else echo $r; fi
}
probe(){ # probe <bin> <run> <srcfile> — echo <归一档>|REJ
  local cb=$1 rb=$2 src=$3
  RUNX "$cb" "$src" "$W/o.elf" >/dev/null 2>&1 || { echo REJ; return; }
  runc "$rb"
}
chk(){ # chk <名> <gcc期望> <cc期望> <min期望> <源码>
  local name=$1 eg=$2 ec=$3 em=$4 src=$5 g c m verdict
  printf '%s' "$src" > "$W/t.c"
  rm -f "$W/gx"
  if gcc -O0 -w -fno-builtin -o "$W/gx" "$W/t.c" 2>/dev/null; then
    bash -c "cd \"$W\" && timeout 5 ./gx >/dev/null 2>&1" 2>/dev/null; local gr=$?
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
chk emptyhex-case     REJ REJ REJ 'int main(){int v;v=1;switch(v){case 0x:return 1;}return 0;}'
# 注：gcc 对未知串转义按字面处理（'a\qb'→q），两侧子集纪律拒——gcc 更宽族台账行
chk SPLIT-G-unkesc   0   REJ REJ 'int main(){char *s;s="a\qb";return 0;}'
chk unkesc-char      122 REJ REJ 'int main(){char c;c='\''\z'\'';return c;}'
chk unclosed-str     REJ REJ REJ 'int main(){char *s;s="ab;return 0;}'
chk unclosed-cmt     REJ REJ REJ 'int main(){return /*'
chk redef-fn         REJ REJ REJ 'int f(){return 1;}int f(){return 2;}int main(){return f();}'
chk uninit-goto-fwd  REJ REJ REJ 'int main(){goto nod;return 0;}'
chk dupcase          REJ REJ REJ 'int main(){int v;v=1;switch(v){case 1:return 1;case 1:return 2;}return 0;}'
chk amp-nonlval      REJ REJ REJ 'int main(){return &3;}'
chk SPLIT-G-fordecl   0   REJ REJ 'int main(){for(int i=0;i<2;i=i+1);return 0;}'
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
# ── cc500 接受面缺口（arity 不校验 / 不做指针性检查 ⇒ 编得过，运行期自成后果）──
# ⚠ arity 行的期望值应取自**调用约定**，不该取自"缺失形参槽里恰好是什么"：
#   cc500 调用方从左到右压参、被调方按「末参 4(%esp) / 首参 8(%esp)」取参 ⇒ 只传一个实参时，
#   该实参落在**末个**形参槽（绑定正确），而**首个**形参槽读的是该处内存原值——实测该值在此
#   程序形状下恒为 124（`return a;`=124、`return a+b;`+f(1)=125、f(7)=131，等价），且在本轮试过的
#   几类扰动（增删局部/调用点前加代码/被调方体加长/形参个数）下都没漂。但它仍**不是语言契约**：
#   换装载栈布局/ABI 细节就可能变，届时报警会指向一处与语义无关的数字。
#   故本行改为读**末个**形参槽 ⇒ 期望 1 由调用约定决定、环境无关。同源正例：形参给齐时三方都是 7。
chk SPLIT-C-arity    REJ 1   REJ 'int f(int a,int b){return b;}int main(){return f(1);}'
chk SPLIT-C-starnum  REJ 165 REJ 'int main(){return *3;}'
echo "----"
if [ $FAILN = 0 ]; then echo "[boundary] PASS($PASS 形态·三方逐位) "; exit 0; fi
echo "[boundary] FAIL: $FAILN 漂移（详见 ↑）"; exit 1
