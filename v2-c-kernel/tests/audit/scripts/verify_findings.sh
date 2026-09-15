#!/usr/bin/env bash
# verify_findings.sh — 逐条可复核：旧发现状态 + 本轮新发现（E 编号与报告 §附录A 对齐）
# 前提：先跑 scripts/build_audit.sh；CC500_SRC 同用。
# 输出每行: E<编号>|<标签>|<实际>|<期望>|<PASS/FAIL>，末行 SUMMARY。
set -u
cd "$(dirname "$0")/.." || exit 1
B=bin; HM=$B/hostcc500; RM=$B/cc500run
[ -x "$HM" ] && [ -x "$RM" ] || { echo "[ERR] 先 bash scripts/build_audit.sh"; exit 2; }
W=$(mktemp -d); pass=0; fail=0
chk() { # chk <E编号> <标签> <期望> <源码>
  local id="$1" tag="$2" want="$3" src="$4"
  printf '%s\n' "$src" > "$W/t.c"
  local out rc got g=""
  out=$("$HM" "$W/t.c" "$W/t.elf" 2>&1); rc=$?
  if [ $rc -ne 0 ]; then
    got="REJ:$(echo "$out" | tr -d '\n' | head -c 28)"
  else
    shift 2; : # unused
    timeout 10 "$RM" "$W/t.elf" >/dev/null 2>&1; local m=$?
    gcc -O0 -std=gnu89 -fno-builtin -w -o "$W/g" "$W/t.c" 2>/dev/null && { timeout 10 "$W/g" >/dev/null 2>&1; g=$?; } || g=NA
    if [ "$want" = "REJ" ]; then got="OK(m=$m)"; else
      got="OK(cc500=$m gcc=$([ "$g" = NA ] && echo NA || echo $((g & 255))))"
    fi
  fi
  if [[ "$got" == "$want"* ]]; then r=PASS; pass=$((pass+1)); else r=FAIL; fail=$((fail+1)); fi
  printf "%-4s|%-22s|%-34s|期望 %-14s|%s\n" "$id" "$tag" "$got" "$want" "$r"
}
echo "== 旧发现复核（期望=当前状态如实快照；修复后应变 FAIL=行为变了，即修复生效）=="
chk E1  "CC-02 大写标识符"      "REJ" 'int main(){int Counter;Counter=3;return Counter-3;}'
chk E2  "CC-03 空语句"          "REJ" 'int main(){;return 0;}'
chk E3  "CC-04 多声明子句"      "REJ" 'int main(){int a,b;return 0;}'
chk E4  "CC-06 字符转义"        "REJ" "int main(){char c;c='\\n';return c-10;}"
chk E5  "CC-07 八进制分歧"      "OK(cc500=10 gcc=8)"        'int main(){int a;a=010;return a;}'
echo "== 新能力正常面（这些应保持 PASS；若变 FAIL=新里程碑回归破坏）=="
chk E6  "M7 三目"               "OK(cc500=0 gcc=0)"         'int main(){int a;a=1;return (a?7:9)-7;}'
chk E7  "M9 hex 小写"           "OK(cc500=0 gcc=0)"         'int main(){int a;a=0x1f;return a-31;}'
chk E8  "M9 hex 大写★缺口"      "REJ"                        'int main(){int a;a=0x1F;return a-31;}'
chk E9  "M9b 复合 /="           "OK(cc500=0 gcc=0)"         'int main(){int a;a=100;a/=5;a+=1;return a-21;}'
chk E10 "M9b <<="               "OK(cc500=0 gcc=0)"         'int main(){int a;a=1;a<<=4;return a-16;}'
chk E11 "M6 短路 &&"            "OK(cc500=0 gcc=0)"         'int main(){int t;t=0;if(0&&(t=1))return 5;return t;}'
chk E12 "M11 goto 后向(小写)"   "OK(cc500=0 gcc=0)"         'int main(){int i;i=0;lp:i=i+1;if(i<3)goto lp;return i-3;}'
chk E13 "M11 goto 前向(小写)"   "OK(cc500=4 gcc=4)"         'int main(){goto ep;return 9;ep:return 4;}'
chk E14 "M11 大写标签★缺口"     "REJ"                       'int main(){int i;i=0;L:i=i+1;if(i<3)goto L;return i-3;}'
echo "== F-01（新发现：M8 前缀 ++/-- 栈记账错位）——期望崩溃即复现 = 当前 bug 存在 =="
chk E15 "M8 前缀语句"           "OK(cc500=139 gcc=2)"       'int main(){int a;a=1;++a;return a;}'
chk E16 "M8 前缀表达式 j=++i"   "OK(cc500=139 gcc=6)"       'int main(){int i;int j;i=5;j=++i;return j;}'
chk E17 "M8 前缀 a_plus_b"      "OK(cc500=139 gcc=4)"       'int main(){int a;int b;a=1;b=2;++a;return a+b;}'
chk E18 "M8 后缀对照(应正常)"   "OK(cc500=255 gcc=255)"       'int main(){int a;int b;a=1;b=2;a++;return a+b-5;}'
echo "SUMMARY pass=$pass fail=$fail （E15-E17 “PASS” 表示 bug 复现仍在）"
