#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/arena/selftest.sh
# agent 演练场 阶段0-2 · 轻量 CI 冒烟（自包含、无 qemu、确定性）。
# 目的：CI 不再依赖既有时序敏感的 `make test`（其 test_socket leak2 偶发会卡住与内核无关的改动），
# 而是单独、可复现地验证 arena 工具链（gate/run/task/qw/evaluate）自身可用。
#
# 用法： bash tests/arena/selftest.sh   （exit 0=全过 / 1=有断言失败）
set -u
cd "$(dirname "$0")" || exit 1
ROOT="$(cd . && pwd)"
VPY="python3"

FAIL=0
ok() { :; }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }
check() { # check <desc> <exit_expected> -- command...
    local desc="$1" want="$2"; shift 2; [ "$1" = "--" ] && shift
    "$@" >/dev/null 2>&1; local got=$?
    if [ "$got" -eq "$want" ]; then echo "  [ok] $desc"; else
        echo "  [FAIL] $desc (exit $got, want $want)"; FAIL=$((FAIL+1)); fi
}

echo "== 0] py_compile 全部模块 =="
for m in gate run task qw evaluate; do
    $VPY -m py_compile "$ROOT/$m.py" && echo "  [ok] $m.py" || { echo "  [FAIL] $m.py"; FAIL=$((FAIL+1)); }
done

echo "== 1] 构造确定性 fixture（akin transcript，契约行/审计均健康）=="
ST=$(mktemp -d)
GOLD="$ST/gold"; BAD="$ST/bad"
for d in "$GOLD" "$BAD"; do mkdir -p "$d"; done
printf '# runid: gold\n# result: PASS\n1\t0\trun isol\n2\t1\trun hello\n' > "$GOLD/in.tr"
printf '1\t0\trun isol\n' > "$BAD/in.tr"
cat > "$GOLD/out.tr" <<'EOF'
[boot] VGA + serial ready
mini-os$ run isol
[shell] 'isol' exited code=0
mini-os$ run hello
[shell] 'hello' exited code=0
ISOLATED OK
writefile '/f1' wrote 42 bytes
[sausage] verify OK
EOF
cp "$GOLD/out.tr" "$BAD/out.tr"                 # bad = gold 完整版再删一条契约行
sed -i "/'hello' exited code=0/d" "$BAD/out.tr" # bad 少 hello 契约 -> contract_content 应 FAIL
printf '# result: PASS\n' > "$GOLD/RESULT"
printf '# result: PASS\n' > "$BAD/RESULT"

echo "== 2] qw.py gates / task list =="
check "gates 列 4 判据" 0 -- $VPY "$ROOT/qw.py" gates
check "task list 至少 1 个" 0 -- $VPY "$ROOT/qw.py" task list

echo "== 3] submit：同契约 PASS(exit0) / 契约丢一行 FAIL(exit1) =="
check "submit gold vs gold -> PASS" 0 -- $VPY "$ROOT/qw.py" submit \
    "$ROOT/tasks/arena-001-torture-a.json" --run "$GOLD" --base "$GOLD"
check "submit bad vs gold -> FAIL(exit1)" 1 -- $VPY "$ROOT/qw.py" submit \
    "$ROOT/tasks/arena-001-torture-a.json" --run "$BAD" --base "$GOLD"

echo "== 4] eval：全判据 closed-loop PASS / 注入 FATAL -> FAIL =="
eval_res=$($VPY "$ROOT/qw.py" eval "$GOLD" --task "$ROOT/tasks/arena-001-torture-a.json" \
    --base "$GOLD" --replay-log "$GOLD/out.tr" 2>/dev/null)
[ "$?" -eq 0 ] && echo "  [ok] eval gold 全 PASS(exit0)" || { echo "  [FAIL] eval gold 未 PASS"; FAIL=$((FAIL+1)); }
echo "$eval_res" | grep -q '"verdict": "PASS"' && echo "  [ok] eval verdict=PASS" \
    || { echo "  [FAIL] eval verdict 非 PASS: $eval_res"; FAIL=$((FAIL+1)); }

FAULT="$ST/fault"
mkdir -p "$FAULT"; cp "$GOLD/out.tr" "$FAULT/out.tr"; cp "$GOLD/in.tr" "$FAULT/in.tr"; cp "$GOLD/RESULT" "$FAULT/RESULT"
printf '\n[FATAL] heap corrupt 0x%x\n' 0 > "$FAULT/audit.txt"
$VPY - "$FAULT/out.tr" "$FAULT/audit.txt" <<'PY'
import sys
with open(sys.argv[1], 'a') as f: f.write("\n[FATAL] heap corrupt at 0x9000\n")
PY
check "eval 注入 FATAL -> FAIL(exit1)" 1 -- $VPY "$ROOT/qw.py" eval "$FAULT" \
    --base "$GOLD" --replay-log "$GOLD/out.tr"

rm -rf "$ST"
echo
if [ "$FAIL" -eq 0 ]; then echo "arena selftest: 全过"; exit 0; else
    echo "arena selftest: $FAIL 处断言失败"; exit 1; fi