#!/usr/bin/env bash
# mc_matrix.sh — minicc MC 回归锁（P0-1 编排层②；语义：gate 已修守卫；新开放项走 XFAIL 告警机制不判红）。
# 由 tests/test_audit.sh 调用（env AUDIT_RUM 在非 ia32-exec 宿主上=「qemu-i386」）。
# ⚠ 维护规则：某项从「未修」翻成「已修」时，把它的 XFAIL 行删除并在上面对应位置补 M/R_ 钉。
set -u
R=$(cd "$(dirname "$0")/.." && pwd)            # = tests/audit
B=$R/bin; H=$B/hostminicc32; U=$B/runmin32
RUM="${AUDIT_RUM:-}"
[ -x "$H" ] && [ -x "$U" ] || { echo "mc_matrix: 先跑 scripts/build_audit.sh（需 MINICC_SRC）"; exit 2; }

GG1(){ printf '%s\n' "$4" > "$W/gg.c"; local rc; RUNX "$3" "$W/gg.c" "$W/gg.elf" >/dev/null 2>&1; rc=$?
  if [ "$rc" = "$2" ]; then ok "$1"; else bad "$1" "rc=$rc 期望=$2"; fi; }
W=$(mktemp -d); FAIL=0
RUNX(){ if [ -n "$RUM" ]; then "$RUM" "$@"; else "$@"; fi; }
ok(){  printf '  %-40s PASS\n' "$1"; }
bad(){ printf '  %-40s FAIL %s\n' "$1" "$2"; FAIL=1; }
M(){   # M <名> <期望编译rc> <源码>
  printf '%s\n' "$3" > "$W/m.c"
  local out rc; out=$(RUNX "$H" "$W/m.c" "$W/m.elf" 2>&1); rc=$?
  if [ "$rc" = "$2" ]; then ok "$1"; else bad "$1" "rc=$rc 期望=$2｜$(printf '%s' "$out" | head -c 44)"; fi
}
R__(){ # R__ <名> <期望运行exit> <源码>（编译+运行双验）
  printf '%s\n' "$3" > "$W/m.c"
  if ! RUNX "$H" "$W/m.c" "$W/m.elf" >"$W/m.out" 2>&1; then bad "$1" "编译意外失败: $(head -c 44 "$W/m.out")"; return; fi
  ( cd "$W" && RUNX "$U" m.elf >/dev/null 2>&1 ); local rc=$?
  if [ "$rc" = "$2" ]; then ok "$1"; else bad "$1" "exit=$rc 期望=$2"; fi
}

# ── MC-04 调用 arity（拒错不误对）────────────────────────
M MC-04-arity不足拒       1 'int f(int a,int b){return a+b;}int main(){int x;x=f(1);return x;}'
M MC-04-arity正确不误伤   0 'int f(int a,int b){return a+b;}int main(){int x;x=f(1,2);return x;}'
# ── MC-07 字面量边界（行为与 gcc 对齐的两处也在）──────────
M MC-07-八进制拒          1 'int main(){int a;a=010;return a;}'
M MC-07-空十六进制拒      1 'int main(){int a;a=0x;return a;}'
R__ MC-07-hex贪心=0x1F    31 'int main(){char *s;s="\x41f";return *(s+0);}'
R__ MC-07-char回绕限幅    44 'int main(){char c;c=300;return c;}'
# ── MC-09 递归深度守卫（EXPR_DEPTH_MAX=32，60 层必受控拒绝、5 层必过）─
O60=$(printf '(%0.s' $(seq 60)); C60=$(printf ')%0.s' $(seq 60))
M MC-09-嵌套60层拒        1 "int main(){int a;a=${O60}1${C60};return a;}"
M MC-09-嵌套5层通过       0 'int main(){int a;a=((((1))));return a;}'
# ── MC-06 源码含 NUL 拒（真实 NUL 字节落盘）────────────────
printf 'int main(){return 0;}\000JUNK' > "$W/nul.c"
out=$(RUNX "$H" "$W/nul.c" "$W/nul.elf" 2>&1); rc=$?
if [ "$rc" = 1 ]; then ok "MC-06-NUL源拒"; else bad "MC-06-NUL源拒" "rc=$rc 期望=1｜$out"; fi
# ── G1：cc500×minicc 数值字面量口径对齐（E5 收口，宿主侧行为钉）────────────
GG1 G1-cc500-010拒       1 "$B/hostcc500" 'int main(){int a;a=010;return a;}'
GG1 G1-cc500-单0不误伤   0 "$B/hostcc500" 'int main(){int a;a=0;return a;}'
GG1 G1-cc500-hex不误伤   0 "$B/hostcc500" 'int main(){int a;a=0x10;return a-16;}'
# ── M7b 声明子句表/空语句（"误拒合法 C"收口；host+self 双实现等值由 heavy 2b4 断言）──
M  M7b-局部子句表接受      0 'int main(){int a,b;a=1;b=2;return a+b-3;}'
R__ M7b-子句初值链行为      0 'int main(){int x=1,y=x+2,z=y+x-4;return z;}'
M  M7b-指针子句接受        0 'int main(){int* p,q;p=0;q=3;return q-3;}'
M  M7b-空语句接受          0 'int main(){int a; ;return a;}'
M  M7b-全局子句表接受      0 'int a[3],b=7;int main(){return b-7;}'
M  M7b-子句内函数声明拒    1 'int f(int a){return a;}int g(),x;'
# ── 自举输入纪律钉：minicc_self.c 必须落在 minicc 自己的可编译子集内（P1 构建=
#    hostminicc 编它；#157 CI 实锤 ternary 越界后补，本地无法执行 P1 时这是唯一早警）──
S1SRC="$(cd "$(dirname "$0")/../../.." && pwd)/tools/minicc/minicc_self.c"
RUNX "$H" "$S1SRC" "$(mktemp -d)/s1.elf" >/dev/null 2>&1 && ok "S1-self源在minicc子集内(P1可构建)" || bad "S1-self源越出minicc子集" "P1 构建将失败——查 minicc_self.c 是否引入 host-only 语法(?:) 等"
# ── MC-08 末角已随上游收口（794a49b：parse 期 defined 标记）→ 原 XFAIL 翻 PASS 钉 ──
# 说明：heavy 层 test_minicc.sh:224-226 已有同形断言（QEMU 路径）；此处为 FAST 宿主层等价钉，
# 两层运行时不同（freestanding 构建 vs in-guest），双保险属 repo 既有分层风格。
M MC-08-函数重定义拒      1 'int f(){return 1;}int f(){return 2;}int main(){int x;x=f();return x;}'
M 冲突钉·变量先行同名函数 1 'int a;int a(){return 1;}int main(){int x;x=2;return x;}'
M 冲突钉·函数先行同名变量 1 'int a(){return 1;}int a;int main(){int x;x=2;return x;}'
M 冲突钉·原型先行同名变量 1 'int a();int a;int main(){int x;x=2;return x;}'
# ── G2：cc500 词法族（M9e 大写标识符/hex/标签、字符转义、// 注释；全走 RUNX）。
#     值探针编译+运行双验（与 verify_findings 的展示级断言互补，此处分身 fast 硬门）──
GG2(){ # GG2 <名> <期望exit> <源码>
  printf '%s\n' "$3" > "$W/g2.c"
  if ! RUNX "$B/hostcc500" "$W/g2.c" "$W/g2.elf" >/dev/null 2>&1; then bad "$1" "cc500 拒编"; return; fi
  ( cd "$W" && RUNX "$B/cc500run" g2.elf >/dev/null 2>&1 ); local rc=$?
  if [ "$rc" = "$2" ]; then ok "$1"; else bad "$1" "exit=$rc 期望=$2"; fi; }
GG2 G2-大写ident值         0 'int main(){int Counter;Counter=5;return Counter-5;}'
GG2 G2-大写hex值          0 'int main(){int a;a=0x1F;return a-31;}'
GG2 G2-大写标签循环        0 'int main(){int i;i=0;L:i=i+1;if(i<3)goto L;return i-3;}'
GG2 G2-char-换行值        0 "int main(){char c;c='\n';return c-10;}"
GG2 G2-x转义值            0 "int main(){char c;c='\x41';return c-65;}"
GG2 G2-行注释在尾          0 'int main(){int x;x=1;return x-1;} //trailing'
# 反例由 GG1(编译rc) 覆盖，此处不重复
# ── G3：cc500 空语句/声明子句表（M9f；含运行值）──
GG2 G3-空语句体            0 'int main(){;return 0;}'
GG2 G3-局部子句表值        0 'int main(){int a,b;a=1;b=2;return a+b-3;}'
GG2 G3-局部初值依赖前子句  0 "int main(){int a=1,b=a+1;return b-2;}"
GG2 G3-全局子句表          0 'int a,b;int main(){a=2;b=3;return a+b-5;}'
GG2 G3-块内空语句          0 'int main(){int a=1;{;};return a-1;}'
# ── G4：cc500 标签终检 + 入口=main（BUG-076/#161、BUG-077/#162；编译 rc 与运行值双钉）──
# #161：未定义标签必须编译期拒（旧码 lbl_end 锚点读错位 → 终检恒假、静默产出跳飞产物）。
GG1 G4-goto未定义拒        1 "$B/hostcc500" 'int main(){goto nod;return 0;}'
GG1 G4-goto定义在后不误伤   0 "$B/hostcc500" 'int main(){goto nod;nod:return 0;}'
# #162：入口=main——main 之前的辅助函数体不得夺走入口（旧码 → main 成死代码，跑出的
# 程序与预期完全不同，曾被误诊为"调用结果进算术算错"）。运值钉=票面三形态 + 入口判别钉。
GG2 G4-调用减-main非首     0 'int f(){return 1;}int main(){return f()-1;}'
GG2 G4-双调用和-main非首   0 'int f(){return 1;}int g(){return 2;}int main(){return f()+g()-3;}'
GG2 G4-调用存取-main非首   0 'int f(){return 1;}int main(){int x;x=f();return x-1;}'
GG2 G4-入口判别-main非首   0 'int f(){return 7;}int main(){return 0;}'
# ── 守卫行 census（第二道保险：整段消失型；阈值=实测留 ~4 行余量）──
n=$(grep -oE 'octal literals not supported|NUL byte in source|arg count mismatch|nesting too deep|fail\("redefined"\)' \
    "$(dirname "$0")/../../../tools/minicc/minicc.c" "$(dirname "$0")/../../../tools/minicc/minicc_self.c" 2>/dev/null | wc -l)
if [ "$n" -ge 24 ]; then ok "守卫行 census（两实现≥24）"; else bad "守卫行 census" "$n/24——有守卫被删"; fi
printf '  mc_matrix 小计：%s\n' "$([ $FAIL = 0 ] && echo 全绿 || echo 有红)"
exit $FAIL
