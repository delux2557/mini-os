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
# ── MC-08 末角已随上游收口（794a49b：parse 期 defined 标记）→ 原 XFAIL 翻 PASS 钉 ──
# 说明：heavy 层 test_minicc.sh:224-226 已有同形断言（QEMU 路径）；此处为 FAST 宿主层等价钉，
# 两层运行时不同（freestanding 构建 vs in-guest），双保险属 repo 既有分层风格。
M MC-08-函数重定义拒      1 'int f(){return 1;}int f(){return 2;}int main(){int x;x=f();return x;}'
M 冲突钉·变量先行同名函数 1 'int a;int a(){return 1;}int main(){int x;x=2;return x;}'
M 冲突钉·函数先行同名变量 1 'int a(){return 1;}int a;int main(){int x;x=2;return x;}'
M 冲突钉·原型先行同名变量 1 'int a();int a;int main(){int x;x=2;return x;}'
# ── 守卫行 census（第二道保险：整段消失型；阈值=实测留 ~4 行余量）──
n=$(grep -oE 'octal literals not supported|NUL byte in source|arg count mismatch|nesting too deep|fail\("redefined"\)' \
    "$(dirname "$0")/../../../tools/minicc/minicc.c" "$(dirname "$0")/../../../tools/minicc/minicc_self.c" 2>/dev/null | wc -l)
if [ "$n" -ge 24 ]; then ok "守卫行 census（两实现≥24）"; else bad "守卫行 census" "$n/24——有守卫被删"; fi
printf '  mc_matrix 小计：%s\n' "$([ $FAIL = 0 ] && echo 全绿 || echo 有红)"
exit $FAIL
