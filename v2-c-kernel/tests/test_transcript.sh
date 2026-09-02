#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_transcript.sh
# record/replay 地基 · P2 验收（transcript 固化）
#
# 验证录制内核（transcript.sh）三个验收点：
#  1. 成功固化：完整录一轮输入/输出 -> build/transcripts/<runid>/ 生成 in.tr/out.tr/RESULT=PASS
#  2. 失败自动归档：人为注入永不命中的期望（DEMO_FAIL=1）-> 触发 tr_abort
#     -> in.tr + out.tr 固化、RESULT=FAIL（"人为触发失败可得可复现归档"）
#  3. 复现性雏形：两次冷启同一命令集，抽公共里程碑行 diff 一致（接 P1 内核确定性）
#
# 不破坏现有回归：独立脚本 + make test-tr。退出码沿用 0/1/2 约定。
set -u
cd "$(dirname "$0")/.." || exit 1
. "$(dirname "$0")/transcript.sh"
for c in qemu-system-i386; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

CFG="-display none -vga std -no-reboot -no-shutdown -m 64 -nic none -serial stdio -monitor none"
# 固定命令集：跨内核子系统的代表性读/运行/检视（复现性比对的"里程碑行"来源）
CMDS=(
  "help"
  "ls"
  "cat motd"
  "run hello"
  "run echo"
  "run isol"
  "run forkdemo"
  "selftest"
)

run_once() {   # run_once <log> <runid>；录制一轮固定命令，串口全量进 out.tr
    local log="$1" runid="$2"
    local TIN="build/${runid}_in.fifo" TOUT="build/${runid}_out.fifo"
    rm -f "$log" "$TIN" "$TOUT"; mkfifo "$TIN" "$TOUT"
    (cat "$TOUT" > "$log") & local cp=$!
    qemu-system-i386 -kernel build/kernel.elf $CFG < "$TIN" > "$TOUT" 2>/dev/null &
    local qp=$!
    exec 9>"$TIN"

    TR_RUNID="$runid" tr_start "$runid"
    export TR_LOG="$log"
    local i
    # 等 shell 提示符（起记时刻后才打点）
    for ((i=0;i<80;i++)); do grep -aq 'mini-os\$ ' "$log" 2>/dev/null && break; sleep 0.25; done
    for c in "${CMDS[@]}"; do
        tr_send "$c"
        sleep 0.4          # 界墙钟：给 shell 处理留给串口输出的相对确定节奏（勿加 icount）
    done
    # BUG-052 残留：固定墙钟快照窗（原 sleep 0.5）会偶发吞掉最后一条命令（selftest）
    # 的尾部输出——`[selftest] PASS` 里程碑未落盘即快照 -> 复现性判据假红（run-a 有、
    # run-b 无）。改以"最后一条命令的完成里程碑落盘"为快照锚点：有界等待；超时也不
    # fail-fast，真缺失仍交由第[4/4]步 diff 如实反映（避免把"捕获窗口"误判成"不
    # 确定"掩盖真回归）。
    local t=0
    while [ "$t" -lt 80 ]; do
        grep -aq '\[selftest\] PASS' "$log" 2>/dev/null && break
        sleep 0.25; t=$((t + 1))
    done
    exec 9>&- 2>/dev/null || true
    kill "$qp" 2>/dev/null || true; wait "$cp" 2>/dev/null || true
    rm -f "$TIN" "$TOUT"
    tr_snapshot "$log"
}

echo "== [1/4] 构建内核 =="
make >/dev/null 2>&1 || { echo "[FAIL] 内核构建失败"; exit 1; }
echo "      构建完成"

echo "== [2/4] 录制一轮（成功固化） =="
rm -rf build/transcripts
run_once build/tr_probe.log run-a
tr_finish 0
[ -s "$TR_DIR/in.tr" ] && [ -s "$TR_DIR/out.tr" ] \
    && [ "$(cat "$TR_DIR/RESULT")" = "# result: PASS" ] \
    && echo "[ok]   固化产物完整 (in.tr=$(wc -l < "$TR_DIR/in.tr")条, out.tr=$(wc -c < "$TR_DIR/out.tr")B)" \
    || { echo "[FAIL] 固化产物不完整"; exit 1; }

echo "== [3/4] 失败自动归档（P2 验收：人为触发失败可得可复现 transcript） =="
DEMO_FAIL="${DEMO_FAIL:-1}"
if [ "$DEMO_FAIL" = "1" ]; then
    # 录制同命令集，但在某处注入一个永不命中的期望 -> 触发 tr_abort
    run_once build/tr_fail.log run-fail
    # 人为"失败点"：造一个缺关键输出的归档（caller 语义：期望 A 未等到 -> tr_abort）
    tr_abort "人为注入：期望 'MINI-OS-NEVER-OUTPUT' 未等到（DEMO_FAIL）" build/tr_fail.log
    [ -s "$TR_DIR/in.tr" ] && [ -s "$TR_DIR/out.tr" ] \
        && grep -q FAIL "$TR_DIR/RESULT" \
        && echo "[ok]   失败现场已固化并标注 FAIL -> $TR_DIR" || { echo "[FAIL] 失败归档缺失"; exit 1; }
fi

echo "== [4/4] 复现性雏形：两次冷启同一命令集，里程碑行逐字节一致 =="
# 第一次已录于 run-a；再录一次 run-b，抽公共里程碑行（去掉动态心跳/pid 相关噪音）
run_once build/tr_probe_b.log run-b
TR_DIR_A="build/transcripts/run-a" TR_DIR_B="build/transcripts/run-b"
# 里程碑行：help 帮助头 + ls 根 + hello 退出 + selftest 汇总 等确定行
#（pin 掉 `ticks=N` 这类墙钟噪音——内核 tick 值在非 icount 下随调度浮动，非语义差异；
#  真逐字节确定性已由 P1 test-det 用 -icount 覆盖，此处只证"里程碑语义行稳定"。）
pick() {
    # 只抽里程碑子串（-oE 逐条输出匹配），不把行内残留字节（如回显 `l` 恰好落在 `[ls] /:` 前）
    # 带入 diff——判据对"行序"敏感、对"行被未完结的前一行残留字节污染"鲁棒。
    # 若里程碑真的缺失（真回归），-oE 同样不输出该子串 -> 仍能由 diff 抓到。
    grep -aoE 'mini-os shell commands:|\[ls\] /:|Hello from \.hello app|\[selftest\] PASS' "$1" \
        | sed 's/ticks=[0-9][0-9]*/ticks=N/'
}
if diff <(pick "$TR_DIR_A/out.tr") <(pick "$TR_DIR_B/out.tr") >/dev/null; then
    echo "[ok]   复现性成立：run-a / run-b 里程碑行逐字节一致"
else
    echo "[FAIL] 复现性不成立（里程碑行有差异，见下）"
    diff <(pick "$TR_DIR_A/out.tr") <(pick "$TR_DIR_B/out.tr") | head -20
    exit 1
fi

echo
echo "P2 验收通过（transcript 固化：输入/输出固化 + 失败自动归档 + 复现性雏形）"
echo "归档目录: build/transcripts/"
exit 0