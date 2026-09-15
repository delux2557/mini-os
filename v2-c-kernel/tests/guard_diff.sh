#!/usr/bin/env bash
# guard_diff.sh — P0-2 守卫行消失审查（advisory：本地 make guard-diff 阻断；CI 步 continue-on-error 提示）。
# 动机（c6c9272 事故型态）：修复守卫连同其测试被"顺手"删掉，行为层测试可能随之蒸发或静默。
# 检测：PR diff 中**净消失**的守卫身份串——编号标记（MC-/CC-/F-/OBS-/BUG-/TD-）与 fail("<消息>")
#       字面。同串若在 + 行重新出现（移位/重构）则不算消失，避免对 P1 重构批误伤。
# 豁免（改钉协议）：PR 正文 / GUARD_DECL 含 `GUARD-CHANGE:` 全局豁免；
#       逐条粒度 `GUARD-CHANGE: <串>` 只豁免匹配项。
# 退出码：0=干净或全豁免；2=存在未声明的守卫消失（=必须回退或补声明）。
set -u
K=$(cd "$(dirname "$0")/.." && pwd); cd "$K" || exit 2
BASE=${1:-}
if [ -z "$BASE" ]; then BASE=$(git merge-base origin/main HEAD 2>/dev/null || true); fi
if [ -z "$BASE" ]; then echo "[guard-diff] ✘ 无 base（传参或保证 origin/main 可 merge-base）"; exit 2; fi
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
git diff -U0 "$BASE"...HEAD -- . | grep '^-' | grep -v '^---'   > "$T/del.txt"
git diff -U0 "$BASE"...HEAD -- . | grep '^+' | grep -v '^+++'   > "$T/add.txt"
MARK='(MC-[0-9]+|CC-[0-9]+|F-[0-9]+|OBS-[0-9]+|BUG-[A-Z0-9]+|TD-[0-9]+|fail\("[^"]*"\))'
grep -oE "$MARK" "$T/del.txt" | sort | uniq -c | sed 's/^ *//' > "$T/delids"
grep -oE "$MARK" "$T/add.txt" | sort -u > "$T/addids"
awk 'NR==FNR{a[$0]=1;next}{id=$2; if(!(id in a)) print $1" "id}' "$T/addids" "$T/delids" > "$T/netgone"
if [ ! -s "$T/netgone" ]; then echo "[guard-diff] ✔ 无守卫身份串净消失"; exit 0; fi
DECL="${GUARD_DECL:-}"
HIT=0; EXEMPT=0
if printf '%s' "$DECL" | grep -q 'GUARD-CHANGE'; then
  while read -r n id; do
    if printf '%s' "$DECL" | grep -q 'GUARD-CHANGE: *all' || printf '%s' "$DECL" | grep -qF -- "${id#fail(}"; then
      EXEMPT=$((EXEMPT+1)); continue
    fi
    printf '  ⚠ 守卫消失 %sx %s —— 未声明\n' "$n" "$id"; HIT=1
  done < "$T/netgone"
else
  while read -r n id; do printf '  ⚠ 守卫消失 %sx %s —— PR 描述需 "GUARD-CHANGE: <原因>" 声明\n' "$n" "$id"; done < "$T/netgone"; HIT=1
fi
if [ $HIT = 1 ]; then echo "[guard-diff] ✘ 未声明的守卫消失（回退，或按改钉协议声明）"; exit 2; fi
echo "[guard-diff] ✔ 守卫消失 ${EXEMPT} 项均已按改钉协议声明"
exit 0
