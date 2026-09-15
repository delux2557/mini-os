#!/usr/bin/env bash
# test-audit（P0-1 外部审计锁）：把四轮审计交付的复测工具固化为 FAST 层常驻门禁。
#   ① cc500 E1-E18 状态快照（打出不 gate）+ F-01 已修行为硬断言
#   ② minicc MC 回归矩阵（tests/audit/scripts/mc_matrix.sh，含行为值钉）
#   ③ cc500 启动不动点 P1==P2（纯宿主，无 QEMU）
# 只 gate「已修守卫」；未来的新开放项按 mc_matrix 顶部规则走 XFAIL 告警（P0-4 已随 794a49b 收口转钉）。
set -u
K=$(cd "$(dirname "$0")/.." && pwd)             # v2-c-kernel
A=$K/tests/audit; B=$A/bin
export CC500_SRC=$K/tools/cc500 MINICC_SRC=$K/tools/minicc
FAIL=0
hdr(){ printf '\n── %s\n' "$1"; }
ok(){  printf '  %-40s PASS (%s)\n' "$1" "$2"; }
bad(){ printf '  %-40s FAIL %s\n' "$1" "$2"; FAIL=1; }

hdr "构建审计工具（freestanding -m32：仅需 gcc 有 -m32 目标，无 32 位 libc/QEMU 要求）"
bash "$A/scripts/build_audit.sh" >/dev/null || { echo "  BUILD FAIL"; exit 2; }

RUM=""
hdr "① cc500 E1-E18 状态快照（verify_findings，仅展示）"
bash "$A/scripts/verify_findings.sh" 2>&1 | tail -2 | sed 's/^/  /'
printf 'int main(){return 7;}\n' > "$B/probe.c"
RUNX(){ if [ -n "$RUM" ]; then "$RUM" "$@"; else "$@"; fi; }
RUNX "$B/hostcc500" "$B/probe.c" "$B/probe.elf" >/dev/null 2>&1
SKIP13=0
probe(){ ( cd "$B" && "$@" ./cc500run probe.elf >/dev/null 2>&1 ); echo $?; }   # 期望 7，非 7=exec 不可
if [ "$(probe)" = 7 ]; then
  RUM=""
elif command -v qemu-i386 >/dev/null 2>&1 && [ "$(probe qemu-i386)" = 7 ]; then
  RUM="qemu-i386"                                                # 与 repo 既有 hostcc 兜底同款（ci-pkgs 自带 qemu-user）
else SKIP13=1; echo "  宿主不能 exec i386 且无 qemu-i386 → ①③ SKIP（②由 mc_matrix 自行硬失败）"; fi
export AUDIT_RUM="$RUM"
if [ "$SKIP13" = 0 ]; then
  for c in 'int main(){int a;a=1;++a;return a;}|2' \
           'int main(){int i;int j;i=5;j=++i;return j;}|6' \
           'int main(){int a;int b;a=1;b=2;++a;return a+b;}|4'; do
    src=${c%|*}; want=${c#*|}
    printf '%s\n' "$src" > "$B/a.c"
    if RUNX "$B/hostcc500" "$B/a.c" "$B/a.elf" >/dev/null 2>&1; then
      ( cd "$B" && RUNX ./cc500run a.elf >/dev/null 2>&1 ); got=$?
    else got=REJ; fi
    if [ "$got" = "$want" ]; then ok "F-01 守卫 exit=$want" "cc500"
    else bad "F-01 守卫" "got=$got 期望=$want ← 已修行为消失，查 PR diff"; fi
  done
fi

hdr "② minicc MC 回归矩阵"
bash "$A/scripts/mc_matrix.sh" || FAIL=1

hdr "③ cc500 启动不动点（185KB 自编译逐字节回环，产物即锁）"
if [ "$SKIP13" = 0 ]; then
  cp "$CC500_SRC/cc500.c" "$B/self.c"
  if RUNX "$B/hostcc500" "$B/self.c" "$B/P1.elf" >/dev/null 2>&1; then
    ( cd "$B" && RUNX ./cc500run P1.elf self.c P2.elf >/dev/null 2>&1 )
    if [ -f "$B/P2.elf" ] && cmp -s "$B/P1.elf" "$B/P2.elf"; then
      ok "启动不动点 P1==P2" "$(stat -c%s "$B/P1.elf") B"
    else bad "启动不动点" "P1!=P2 或 P2 未产出——启动断裂/产物被改变，PR 须显式声明"; fi
  else bad "启动不动点" "P1 编译失败"; fi
fi

printf '\n[test-audit] %s\n' "$([ $FAIL = 0 ] && echo '✔ 全绿——已修守卫完好' || echo '✘ 见上 FAIL 行——对照四轮审计修复点排查（合法行为变更需在 PR 描述声明并同步改钉）')"
exit $FAIL
