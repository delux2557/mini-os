#!/usr/bin/env bash
# test-audit（P0-1 外部审计锁）：把四轮审计交付的复测工具固化为 FAST 层常驻门禁。
#   ① cc500 E1-E18 一致性快照（verify_findings，期望=修复后行为，仅展示）+ F-01 硬断言×3
#   ② minicc MC 回归矩阵 13 钉（tests/audit/scripts/mc_matrix.sh，含行为值钉）
#   ③ cc500 启动不动点 P1==P2（纯宿主回环，无 qemu-system）
# 只 gate「已修守卫」；未来新开放项按 mc_matrix 顶部规则走 XFAIL 告警（P0-4 已随 794a49b 收口转钉）。
# 环境要求：构建需 gcc 的 -m32 目标 + gcc-multilib 头（与本仓 test-cc500 同源）；运行需原生 ia32
#           exec 或 qemu-user（ci-pkgs 均含）。exec 探测先于一切产物依赖（dev 反馈修正：消除 RUM
#           循环）；两条通道皆不可用时 exit 2 显式拒跑——不静默 SKIP，也不把"环境不可用"误报成守卫红。
set -u
K=$(cd "$(dirname "$0")/.." && pwd)             # v2-c-kernel
A=$K/tests/audit; B=$A/bin
export CC500_SRC=$K/tools/cc500 MINICC_SRC=$K/tools/minicc
FAIL=0
hdr(){ printf '\n── %s\n' "$1"; }
ok(){  printf '  %-40s PASS (%s)\n' "$1" "$2"; }
bad(){ printf '  %-40s FAIL %s\n' "$1" "$2"; FAIL=1; }
RUM=""
RUNX(){ if [ -n "$RUM" ]; then "$RUM" "$@"; else "$@"; fi; }

hdr "构建审计工具（freestanding -m32；构建面同 test-cc500，需 gcc-multilib）"
bash "$A/scripts/build_audit.sh" >/dev/null || { echo "  BUILD FAIL"; exit 2; }

# ── exec 能力探测：用构建器自带的 cc500run「无参用法退出码」探路（2/3，与 exec 失败的 126 正交），
#    不依赖任何先编译产物 → 无循环。 ──
( cd "$B" && ./cc500run >/dev/null 2>&1 ); rc=$?
if [ "$rc" != 126 ]; then
  RUM=""
elif command -v qemu-i386 >/dev/null 2>&1; then
  ( cd "$B" && qemu-i386 ./cc500run >/dev/null 2>&1 ); rc2=$?
  if [ "$rc2" != 126 ]; then RUM="qemu-i386"; fi
else
  : # 两路皆无 → 下方拒跑
fi
if [ -z "$RUM" ] && [ "$rc" = 126 ]; then
  echo "  ✘ 需要 ia32 exec 或 qemu-i386（ci-pkgs 均含）——本层拒绝在盲跑中给出红/绿"; exit 2
fi
export AUDIT_RUM="$RUM"
[ -n "$RUM" ] && echo "  exec 通道：$RUM"

hdr "① cc500 E1-E18 一致性快照（期望=修复后行为；与 test-cc500 互补，仅展示不 gate）"
bash "$A/scripts/verify_findings.sh" 2>&1 | tail -2 | sed 's/^/  /'
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

hdr "② minicc MC 回归矩阵"
bash "$A/scripts/mc_matrix.sh" || FAIL=1

hdr "③ cc500 启动不动点（185KB 自编译逐字节回环，产物即锁）"
cp "$CC500_SRC/cc500.c" "$B/self.c"
if RUNX "$B/hostcc500" "$B/self.c" "$B/P1.elf" >/dev/null 2>&1; then
  ( cd "$B" && RUNX ./cc500run P1.elf self.c P2.elf >/dev/null 2>&1 )
  if [ -f "$B/P2.elf" ] && cmp -s "$B/P1.elf" "$B/P2.elf"; then
    ok "启动不动点 P1==P2" "$(stat -c%s "$B/P1.elf") B"
  else bad "启动不动点" "P1!=P2 或 P2 未产出——启动断裂/产物被改变，PR 须显式声明"; fi
else bad "启动不动点" "P1 编译失败"; fi

printf '\n[test-audit] %s\n' "$([ $FAIL = 0 ] && echo '✔ 全绿——已修守卫完好' || echo '✘ 见上 FAIL 行——对照四轮审计修复点排查（合法行为变更需在 PR 描述声明并同步改钉）')"
exit $FAIL
