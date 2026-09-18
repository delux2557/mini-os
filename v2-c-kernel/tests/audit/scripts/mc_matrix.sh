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
# ── 死钉自检设施（#165 复核实测发现）──
# 原 G5 组在 GG2 **定义之前**就调用了它：bash 只往 stderr 丢一行 `command not found`，而本脚本
# 无 set -e、调用方也不查 stderr ⇒ 该钉从未执行、小计照报「全绿」（#166 加的判别力钉一直是死的）。
# 现把整脚本 stderr 收进日志（原 stderr 存 fd 3 以便末尾原样回放），末尾检出该串即判红。
ERRLOG=$(mktemp); exec 3>&2; exec 2>"$ERRLOG"
GG2(){ # GG2 <名> <期望exit> <源码>——cc500 编译+运行双验（与 GG1 的仅编译 rc 面互补）
  printf '%s\n' "$3" > "$W/g2.c"
  if ! RUNX "$B/hostcc500" "$W/g2.c" "$W/g2.elf" >/dev/null 2>&1; then bad "$1" "cc500 拒编"; return; fi
  ( cd "$W" && RUNX "$B/cc500run" g2.elf >/dev/null 2>&1 ); local rc=$?
  if [ "$rc" = "$2" ]; then ok "$1"; else bad "$1" "exit=$rc 期望=$2"; fi; }
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
BOTH(){ # BOTH <名> <minicc侧期望> <源码> [cc500侧期望，缺省同 minicc侧]——**同一份源码**两侧各编各跑。
  # 「对称」两字只能这样证：单侧钉无法区分「两边都对」与「两边各错一半」。第 4 参给出时=**有意
  # 分歧钉**（cc500 无类型面导致的语义差），两侧各自比对 ⇒ 分歧被显式锁住，任一侧漂移即红。
  printf '%s\n' "$3" > "$W/b.c"
  local mc cc
  if RUNX "$H" "$W/b.c" "$W/b.mc" >/dev/null 2>&1; then ( cd "$W" && RUNX "$U" b.mc >/dev/null 2>&1 ); mc=$?
  else mc=REJ; fi
  if RUNX "$B/hostcc500" "$W/b.c" "$W/b.500" >/dev/null 2>&1; then ( cd "$W" && RUNX "$B/cc500run" b.500 >/dev/null 2>&1 ); cc=$?
  else cc=REJ; fi
  local want_m="$2" want_c="${4:-$2}"
  if [ "$mc" = "$want_m" ] && [ "$cc" = "$want_c" ]; then ok "$1"
  else bad "$1" "minicc=$mc(期望$want_m) cc500=$cc(期望$want_c)"; fi
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
# ── G6：双编译器同理由必拒负例（P0-6 收尾）。含 cc500 空 hex 修复钉——`0x;` 曾静默算 0
#    （MC-07#5 对齐时的 cc500 侧漏项，本 PR 修 primary_expr 与 sw_const 两处解析点）──
GG1 G6-cc空hex拒        1 "$B/hostcc500" 'int main(){int a;a=0x;return a;}'
GG1 G6-cc空hex-case拒   1 "$B/hostcc500" 'int main(){int v;v=1;switch(v){case 0x:return 1;}return 0;}'
GG1 G6-cc重定义拒       1 "$B/hostcc500" 'int f(){return 1;}int f(){return 2;}int main(){return 0;}'
GG1 G6-cc未知转义拒     1 "$B/hostcc500" 'int main(){char *s;s="a\qb";return 0;}'
GG1 G6-cc未闭合串拒     1 "$B/hostcc500" 'int main(){char*s;s="abc;return 0;}'
GG1 G6-cc未闭合注释拒   1 "$B/hostcc500" 'int main(){return /*'
GG1 G6-min空hex拒       1 "$H" 'int main(){int a;a=0x;return a;}'

# ── G5：cc500 字符串转义 M9g（收口面钉；字节语义=golden g10/g11 哈希 + verify E20/E21）──
# ⚠ 本组必须走 GG1/GG2（hostcc500/cc500run）：M9g 改的是 **cc500** 侧，而 `M()` 跑的是
#   hostminicc32（minicc 未动）——原版误挂在 M() 上，修复前后结果相同=零判别力（复核实测）。
#   minicc 侧的转义契约另有 test_minicc.sh 覆盖（t_badesc 拒未知转义 / t_xesc \x 贪心），不在此重复。
# 钉②用字节探针而非"能否编译"：`s="a\\b"` 新旧都编得过，仅**解码字节**不同（旧留两个反斜杠），
#   故查 p[2]=='b' 才对得上——纯编译 rc 钉对这条形态无判别力（复核实测）。
GG1 G5-cc500-未知转义拒    1 "$B/hostcc500" 'int main(){char *s;s="a\qb";return 0;}'
GG2 G5-cc500-双反斜字宽   0 'int main(){char *p;p="a\\b";if(p[2]==98)return 0;return 1;}'

# ── G7：#165 指针访问语法双向对称（cc500 补一元 * 与 &，minicc 补 p[i] 脱糖）──
# 票面《cc500×minicc 指针访问语法双向不对称》：cc500 无 unary *（读+写），minicc 无指针下标。
# 收口后四种形态 × 读写位在两侧同源同值；验收义务「对称矩阵」的 FAST 硬门就落在下面 BOTH 钉上。
# 说明：differ 钉只用 `char *` 串/局部（两编译器宽度口径一致的那一半），`int *` 的宽度有意
# 分歧单独钉在组末（见其注释），不混进对称矩阵以免"把已知分歧算成对称"。
BOTH G7-解引用读          0 'int main(){char *s;int x;s="abc";x=*s;return x-97;}'
BOTH G7-解引用写          0 'int main(){char *s;s="abc";*s=65;return *s-65;}'
BOTH G7-解引用偏移读      0 'int main(){char *s;int x;s="abc";x=*(s+1);return x-98;}'
BOTH G7-解引用偏移写      0 'int main(){char *s;s="abc";*(s+1)=66;return *(s+1)-66;}'
BOTH G7-下标读            0 'int main(){char *s;int x;s="abc";x=s[1];return x-98;}'
BOTH G7-下标写            0 'int main(){char *s;s="abc";s[1]=66;return s[1]-66;}'
BOTH G7-取址后解引用      0 'int main(){int a;int *p;a=7;p=&a;return *p-7;}'
BOTH G7-取址非左值双拒    REJ 'int main(){int a;a=&3;return a;}'
# 有意分歧钉（**非缺陷**，锁住"分歧是刻意保留的"）：cc500 无类型面 ⇒ deref 恒 char 宽（movsbl），
# minicc 有类型面 ⇒ 按 int 宽取。同一份源码两侧各自比对，任一侧漂移即红。
# a=200 的低字节 C8 作有符号取 = -56 < 0 ⇒ cc500 返 7；minicc 读到 200 ⇒ 返 8。
BOTH G7-有意分歧-int宽    8 'int main(){int a;int*p;a=200;p=&a;if(*p<0)return 7;return 8;}' 7

# ── 自举输入纪律钉：minicc_self.c 必须落在 minicc 自己的可编译子集内（P1 构建=
#    hostminicc 编它；#157 CI 实锤 ternary 越界后补，本地无法执行 P1 时这是唯一早警）──
S1SRC="$(cd "$(dirname "$0")/../../.." && pwd)/tools/minicc/minicc_self.c"
S1ELF="$W/s1self.elf"
if RUNX "$H" "$S1SRC" "$S1ELF" >/dev/null 2>&1; then ok "S1-self源在minicc子集内(P1可构建)"
else bad "S1-self源越出minicc子集" "P1 构建将失败——查 minicc_self.c 是否引入 host-only 语法(?:) 等"; fi
# ── S2：自举容量纪律钉（#172）──
# #172 实录：自举不动点曾因"两处按 40KB 源定下的常量"而**静默断掉**且无人知（miccboot 无 CI 层）。
# 本组把"源涨过头"从慢层的静默失败提前成 FAST 层的显式红；常量一律从**自举源里读**（单一事实
# 源，改源即改门禁，不在此手抄数值）。能静态判定的三条在此；节点峰值只能实测 ⇒ miccboot 层兜底。
sget(){ sed -nE "s/^[[:space:]]*(int[[:space:]]+)?$1[[:space:]]*=[[:space:]]*([0-9]+).*/\2/p" "$S1SRC" | head -1; }
S_ICAP=$(sget IN_CAP); S_ILIMIT=$(sget IN_LIMIT); S_NMAX=$(sget NMAX); S_CCAP=$(sget code_cap)
S_SRC=$(wc -c < "$S1SRC" | tr -d ' ')
# S2a：读块余量关系——in 是 xmalloc(IN_CAP)、读块固定 4096；差额不足则"in_len 刚过上限"那次读
#      会越界写缓冲（旧码 65536/65536 正是缺这个差额，即上限与容量同值）。
if [ -n "$S_ICAP" ] && [ -n "$S_ILIMIT" ] && [ $((S_ICAP - S_ILIMIT)) -ge 4096 ]; then
  ok "S2a-读块余量($((S_ICAP - S_ILIMIT))≥4096)"
else bad "S2a-读块余量不足" "IN_CAP=$S_ICAP IN_LIMIT=$S_ILIMIT（须差 ≥4096）"; fi
# S2b：源体积 ≤ 75% IN_LIMIT（留 ≥25% 生长余量）——逼近即先红，不等到自举不过才发现。
if [ -n "$S_ILIMIT" ] && [ "$S_SRC" -le $((S_ILIMIT * 3 / 4)) ]; then
  ok "S2b-自举源余量($S_SRC≤$((S_ILIMIT * 3 / 4)))"
else bad "S2b-自举源逼近输入上限" "源 $S_SRC vs IN_LIMIT $S_ILIMIT（须 ≤75%）"; fi
# S2c：**实测**产物 < 0.9×code_cap。必须实测不能估算：产物长度直接决定 P1 能否产出 P2
#      （emit1 不扩容 = 产物硬上限），而它同时随 NMAX（零初始化全局数组内联进产物，
#       每节点 +42 B）与代码量增长——#172 里"抬 NMAX 会挤爆 code_cap"正踩在这条耦合上。
if [ -s "$S1ELF" ] && [ -n "$S_CCAP" ]; then
  S_PRD=$(wc -c < "$S1ELF" | tr -d ' ')
  if [ "$S_PRD" -lt $((S_CCAP * 9 / 10)) ]; then
    ok "S2c-自举产物余量($S_PRD<$((S_CCAP * 9 / 10)))"
  else bad "S2c-自举产物逼近 code_cap" "产物 $S_PRD vs code_cap $S_CCAP（须 <90%）"; fi
else bad "S2c-无法测量自举产物" "S1 未产出产物，或读不到 code_cap=$S_CCAP"; fi
if [ -n "$S_NMAX" ] && [ "$S_NMAX" -gt 8192 ]; then ok "S2d-节点池未被回退($S_NMAX>8192)"
else bad "S2d-节点池过小" "NMAX=$S_NMAX（#172 前值 8192 已不够）"; fi
# S2e：名字池容量 ≥ 源体积 × 0.5（实测用量 ≈ 源 × 0.28 ⇒ 该式保底 ≥1.8× 余量）。名字池曾是
#      **无守卫的静态池**且已用到 90%（18.5KB/20.5KB）：溢出会静默写穿相邻内存、报出与真因无关
#      的错——#172 里那个"too many nodes"的真身就是它（见 minicc_self.c 「自举容量」④与订正段）。
#      现容量动态、越界由 stradd 受控报错；本钉防的正是"容量又被改小/源涨过头"这类静默回归。
S_STRCAP=$(sget STRTAB_CAP)
if [ -n "$S_STRCAP" ] && [ "$S_STRCAP" -ge $((S_SRC / 2)) ]; then
  ok "S2e-名字池余量($S_STRCAP≥$((S_SRC / 2)))"
else bad "S2e-名字池容量不足" "STRTAB_CAP=$S_STRCAP vs 源 $S_SRC（须 ≥ 源/2）"; fi
# ── M7d 同作用域重声明（F7/#186）：三条受控拒 + 两条"遮蔽合法"哨兵（防过度收紧）──
M  M7d-同子句重名拒        1 'int main(){int a=1,a;return a-1;}'
M  M7d-同块两语句重名拒    1 'int main(){int a;int a;a=2;return a-2;}'
M  M7d-形参重名拒          1 'int f(int a,int a){return a;}int main(){return 0;}'
R__ M7d-嵌套块遮蔽不误伤   0 'int main(){int a;a=1;{int a;a=5;return a-5;}}'
R__ M7d-局部遮蔽全局函数名 0 'int rel(){return 7;}int main(){int rel;rel=3;return rel-3;}'

# ── MC-08 末角已随上游收口（794a49b：parse 期 defined 标记）→ 原 XFAIL 翻 PASS 钉 ──
# 说明：heavy 层 test_minicc.sh:224-226 已有同形断言（QEMU 路径）；此处为 FAST 宿主层等价钉，
# 两层运行时不同（freestanding 构建 vs in-guest），双保险属 repo 既有分层风格。
M MC-08-函数重定义拒      1 'int f(){return 1;}int f(){return 2;}int main(){int x;x=f();return x;}'
M 冲突钉·变量先行同名函数 1 'int a;int a(){return 1;}int main(){int x;x=2;return x;}'
M 冲突钉·函数先行同名变量 1 'int a(){return 1;}int a;int main(){int x;x=2;return x;}'
M 冲突钉·原型先行同名变量 1 'int a();int a;int main(){int x;x=2;return x;}'
# ── G2：cc500 词法族（M9e 大写标识符/hex/标签、字符转义、// 注释；全走 RUNX）。
#     值探针编译+运行双验（与 verify_findings 的展示级断言互补，此处分身 fast 硬门）──
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
# ── 死钉自检（#165 复核实测发现：#166 加的 G5 判别力钉因 GG2 定义滞后而从未执行）──
# 检出"命令未找到"即判红；同时把整段 stderr 回放到 fd 3（原始 stderr），诊断不丢失。
if grep -q 'command not found' "$ERRLOG"; then
  bad "死钉自检·断言函数定义顺序" "$(grep -m1 'command not found' "$ERRLOG")"
fi
cat "$ERRLOG" >&3; exec 2>&3; rm -f "$ERRLOG"
# ── 守卫行 census（第二道保险：整段消失型；阈值=实测留 ~4 行余量）──
n=$(grep -oE 'octal literals not supported|NUL byte in source|arg count mismatch|nesting too deep|fail\("redefined"\)|subscript of non-pointer' \
    "$(dirname "$0")/../../../tools/minicc/minicc.c" "$(dirname "$0")/../../../tools/minicc/minicc_self.c" 2>/dev/null | wc -l)
if [ "$n" -ge 26 ]; then ok "守卫行 census（两实现≥26）"; else bad "守卫行 census" "$n/26——有守卫被删"; fi
printf '  mc_matrix 小计：%s\n' "$([ $FAIL = 0 ] && echo 全绿 || echo 有红)"
exit $FAIL
