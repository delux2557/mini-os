#!/usr/bin/env bash
# guard_diff.sh — P0-2 守卫消失审查（advisory 起步→稳定后转阻断；dev 复核后 v2）。
# 检测：PR diff 中**净消失**的守卫身份串——编号标记（MC-/CC-/F-/OBS-/BUG-/TD-）与 fail("<消息>")
#       字面。同串在 + 行重现（移位/重构）不算消失。
# 豁免（改钉协议）：独占一行（或 markdown 引用行）的
#       `GUARD-CHANGE: all`            → 全局豁免
#       `GUARD-CHANGE: <core 消息>`    → 逐条豁免（core=fail(...) 的引号内消息裸串/编号原串）
# 退出码：0 干净或全豁免；2 未声明的消失 或 base 不可达（浅克隆不再恒绿，dev 复核 P0-B）。
# 注意：本脚本自身被改动时会打 ⚠ 提示（exclude 自扫描的代价补偿，dev 复核 P2-E）。
set -u
K=$(cd "$(dirname "$0")/.." && pwd); cd "$K" || exit 2
BASE=${1:-}
if [ -z "$BASE" ]; then BASE=$(git merge-base origin/main HEAD 2>/dev/null || true); fi
if [ -z "$BASE" ] || ! git cat-file -e "${BASE}^{commit}" 2>/dev/null; then
  echo "[guard-diff] ✘ base 不可达（浅克隆？显式传参 BASE 或 fetch 完整历史），拒绝在无审查窗口时给出 ✔"
  exit 2
fi
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
RANGE="$BASE...HEAD"
git diff -U0 "$RANGE" -- . ":(exclude)tests/guard_diff.sh" | grep '^-' | grep -v '^---' > "$T/del.txt"
git diff -U0 "$RANGE" -- . ":(exclude)tests/guard_diff.sh" | grep '^+' | grep -v '^+++' > "$T/add.txt"
if git diff --name-only "$RANGE" | grep -qE 'tests/guard_diff\.sh$|\.github/workflows/layers\.yml$'; then
  echo "  ⚠ 本审查工具或其 CI 步在本次 diff 中被修改——豁免判定请人工 double-check（advisory，不改变退出码）"
fi
MARK='(MC-[0-9]+|CC-[0-9]+|F-[0-9]+|OBS-[0-9]+|BUG-[A-Z0-9]+|TD-[0-9]+|fail\("[^"]*"\))'
grep -oE "$MARK" "$T/del.txt" | sort | uniq -c | sed 's/^ *//' > "$T/delids"
grep -oE "$MARK" "$T/add.txt" | sort -u > "$T/addids"
# sentinel 行：防两文件式 awk 在 addids 为空时对第二文件 NR==FNR 恒真（dev 复核 P0-A：
# 该陷阱此前"修在 commit message、没进代码"——此版本随首个 demo 一起从提交通道复验）。
printf '_sentinel\n' >> "$T/addids"
awk 'NR==FNR{a[$0]=1;next}{id=substr($0,index($0," ")+1); if(!(id in a)) print id}' "$T/addids" "$T/delids" | sort > "$T/netgone"
if [ ! -s "$T/netgone" ]; then echo "[guard-diff] ✔ 无守卫身份串净消失"; exit 0; fi
DECL="${GUARD_DECL:-}"
# 只认锚定的声明行：可选引用前缀 ">"，整行仅 GUARD-CHANGE: <payload>（防 PR 正文说明性
# 提及含 'GUARD-CHANGE: all' 字面而自我全豁免，dev 复核 P1-C）。
printf '%s\n' "$DECL" | sed -nE 's/^[[:space:]>]*GUARD-CHANGE:[[:space:]]*(.*)$/\1/p' \
  | sed -E 's/[[:space:]]+$//' > "$T/decls"
HIT=0; EXEMPT=0
if grep -qxE 'all' "$T/decls" 2>/dev/null; then
  EXEMPT=$(wc -l < "$T/netgone" | tr -d ' ')
  echo "[guard-diff] ✔ GUARD-CHANGE: all 声明在案，守卫消失 $(grep -c '' "$T/netgone" | tr -d ' ') 项全豁免（review 须核对声明与 diff 相符）"
  exit 0
fi
while IFS= read -r id; do
  core=$(printf '%s' "$id" | sed -nE 's/^fail\("(.*)"\)$/\1/p')
  [ -z "$core" ] && core=$id                       # 非 fail(…) 形态：原串即匹配键
  if grep -qxF -- "$core" "$T/decls"; then
    EXEMPT=$((EXEMPT+1))
  else
    printf '  ⚠ 守卫消失 %s —— 未声明（补独占一行 "GUARD-CHANGE: %s"）\n' "$id" "$core"; HIT=1
  fi
done < "$T/netgone"
if [ "$HIT" = 1 ]; then echo "[guard-diff] ✘ 存在未声明的守卫消失"; exit 2; fi
echo "[guard-diff] ✔ 守卫消失 ${EXEMPT} 项均逐条声明"
exit 0
