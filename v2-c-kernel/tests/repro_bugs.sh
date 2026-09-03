#!/usr/bin/env bash
# 实锤复现 BUG-A（文件槽泄漏污染工具链）与 BUG-B（cc500 产物丢失 exec argv）
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/_build_env.sh"   # BUG-053：先定 BUILD，transcript 默认读写随其走
source "$SCRIPT_DIR/transcript.sh"   # record/replay · P2 录制：把下面复现命令流固化为 .in.tr/.out.tr
cd "$SCRIPT_DIR/.." || exit 1        # 相对路径定位到 v2-c-kernel/，任意机器可跑
make BUILD="$BUILD" >/dev/null 2>&1
LOG="$BUILD/repro.log"
export TR_LOG="$LOG"
TIN="$BUILD/repro_in.fifo"
TOUT="$BUILD/repro_out.fifo"
QPID=""; CAT_PID=""
FAIL=0
cleanup() { exec 9>&- 2>/dev/null || true; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true; rm -f "$TIN" "$TOUT"; }
trap cleanup EXIT
rm -f "$LOG" "$TIN" "$TOUT"
mkfifo "$TIN" "$TOUT"
(cat "$TOUT" > "$LOG") & CAT_PID=$!
qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none -vga std -no-reboot -no-shutdown -m 64 -nic none -serial stdio -monitor none < "$TIN" > "$TOUT" 2>/dev/null &
QPID=$!
exec 9>"$TIN"

wait_for() { local desc="$1" re="$2" tmo="${3:-8}" i; for ((i=0;i<tmo*4;i++)); do grep -aq "$re" "$LOG" 2>/dev/null && { echo "[ok]   $desc"; return 0; }; sleep 0.25; done; echo "[FAIL] $desc (缺: $re)"; FAIL=$((FAIL+1)); return 1; }
send() { tr_send "$1"; sleep 0.3; }   # tr_send：写 fd9 并录制进 in.tr（含相对 ms），保留 0.3s 打拍

wait_for "shell 提示符" "mini-os\$ " 20
tr_start repro                          # 录制起点（起记相对 ms）
tr_snapshot "$LOG"
tr_mark ready "$LOG"                    # F4：就绪锚——之后判据一律在锚后归一化（不扫 boot 期输出）

echo "===== BUG-B：cc500 自编译产物丢 argv ====="
send "ccboot"
wait_for "自举 P1/P2 完成" "\[ccboot\] byte-identical PASS" 60

# 用 argv 路径编译：期望 /cc500.c -> /out2.elf；若 argv 丢失则静默写默认 /out.elf
send "exec /out.elf /cc500.c /out2.elf"
wait_for "P1 编译（应走默认路径）" "cc500: compiled OK" 60
sleep 1
send "ls"
wait_for "ls 输出" "\[ls\] /:" 8
# F6（交接单 处理项）：exec argv 生效专项断言。旧断言 grep "out2.elf" 会误匹配 shell 回显的命令本身，
# 且为软分支（argv 回归也永不红）。改为按 `[ls]   out2.elf ...` 目录条目判定：仅当 argv 生效、文件真实
# 落盘才 ok；argv 被静默丢弃（cc500 写默认 /out.elf）则必然红。
# F4：判据经 tr_window_after ready 只在就绪锚后扫描，归一化，杜绝 boot 期同名输出误匹配。
if tr_window_after ready "$LOG" | grep -aqE "\[ls\]  *out2\.elf"; then
    echo "[ok]   argv 生效：/out2.elf 已在 ls 中列出（BUG-B 未复现）"
else
    echo "[!]   BUG-B 复现：ls 无 /out2.elf —— exec argv 被静默丢弃，cc500 落默认 /out.elf"
    FAIL=$((FAIL + 1))
fi

echo "===== BUG-A：编译失败泄漏 slot2 -> 后续编译判若两机 ====="
GOOD='int syscall3(int n,int a,int b,int c);int main(){return 0;}'
send "writefile /good.c $GOOD"
wait_for "写 good.c" "wrote [0-9][0-9]* bytes"
send "ccrun /good.c /good.elf"
wait_for "首次编译 OK" "cc500: compiled OK" 30
wait_for "首次运行 PASS" "'/good.elf' exited code=0 PASS"

# 语法拒绝源（全局数组，文法必然 error()）
send 'writefile /bad.c int arr[4];'
wait_for "写 bad.c" "wrote [0-9][0-9]* bytes"
send "ccrun /bad.c /bad.elf"
wait_for "坏源编译 FAIL code=1" "\[ccrun\] compile FAIL code=1" 30

# 字节级一致的源再次编译：应仍成功；若 slot2 泄漏则 setup_output 失败
send "writefile /good2.c $GOOD"
wait_for "写 good2.c" "wrote [0-9][0-9]* bytes"
send "ccrun /good2.c /good2.elf"
if grep -aq "output setup fail" "$LOG"; then
    echo "[!]   BUG-A 属实：同源二次编译失败（cc500: output setup fail = slot2 泄漏污染）"
    wait_for "二次编译 FAIL code=1" "\[ccrun\] compile FAIL code=1" 15
else
    echo "[ok]   BUG-A 未复现：二次编译仍成功"
fi

sleep 1
exec 9>&- 2>/dev/null || true
# 录制收尾：快照输出进 out.tr，并按 FAIL 状态标 RESULT（0=无回归 / 1=复现出现）
tr_snapshot "$LOG"
tr_finish $(( FAIL==0 ? 0 : 1 ))
kill "$QPID" 2>/dev/null || true; wait "$CAT_PID" 2>/dev/null || true
echo
echo "==== 复现日志关键行 ===="
grep -aE "ccrun|cc500:|exec|byte-identical|out2.elf|good2|writefile" "$LOG" | tail -30
exit 0
