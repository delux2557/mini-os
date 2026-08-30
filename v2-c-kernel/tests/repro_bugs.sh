#!/usr/bin/env bash
# 实锤复现 BUG-A（文件槽泄漏毒化工具链）与 BUG-B（cc500 产物丢失 exec argv）
set -u
cd "$(dirname "$0")/.." || exit 1   # 相对路径定位到 v2-c-kernel/，任意机器可跑
make >/dev/null 2>&1
LOG=build/repro.log
TIN=build/repro_in.fifo
TOUT=build/repro_out.fifo
QPID=""; CAT_PID=""
FAIL=0
cleanup() { exec 9>&- 2>/dev/null || true; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true; rm -f "$TIN" "$TOUT"; }
trap cleanup EXIT
rm -f "$LOG" "$TIN" "$TOUT"
mkfifo "$TIN" "$TOUT"
(cat "$TOUT" > "$LOG") & CAT_PID=$!
qemu-system-i386 -kernel build/kernel.elf -display none -vga std -no-reboot -no-shutdown -m 64 -serial stdio -monitor none < "$TIN" > "$TOUT" 2>/dev/null &
QPID=$!
exec 9>"$TIN"

wait_for() { local desc="$1" re="$2" tmo="${3:-8}" i; for ((i=0;i<tmo*4;i++)); do grep -aq "$re" "$LOG" 2>/dev/null && { echo "[ok]   $desc"; return 0; }; sleep 0.25; done; echo "[FAIL] $desc (缺: $re)"; FAIL=$((FAIL+1)); return 1; }
send() { printf '%s\n' "$1" >&9; sleep 0.3; }

wait_for "shell 提示符" "mini-os\$ " 20

echo "===== BUG-B：cc500 自编译产物丢 argv ====="
send "ccboot"
wait_for "自举 P1/P2 完成" "\[ccboot\] byte-identical PASS" 60

# 用 argv 路径编译：期望 /cc500.c -> /out2.elf；若 argv 丢失则静默写默认 /out.elf
send "exec /out.elf /cc500.c /out2.elf"
wait_for "P1 编译（应走默认路径）" "cc500: compiled OK" 60
sleep 1
send "ls"
wait_for "ls 输出" "\[ls\] /:" 8
if grep -aq "out2.elf" "$LOG"; then
    echo "[ok]   BUG-B 不存在：/out2.elf 已被创建（argv 生效）"
else
    echo "[!]   BUG-B 属实：/out2.elf 未创建（exec argv 被静默丢弃，P1 用默认路径写 /out.elf）"
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
    echo "[!]   BUG-A 属实：同源二次编译失败（cc500: output setup fail = slot2 泄漏毒化）"
    wait_for "二次编译 FAIL code=1" "\[ccrun\] compile FAIL code=1" 15
else
    echo "[ok]   BUG-A 未复现：二次编译仍成功"
fi

sleep 1
exec 9>&- 2>/dev/null || true
kill "$QPID" 2>/dev/null || true; wait "$CAT_PID" 2>/dev/null || true
echo
echo "==== 复现日志关键行 ===="
grep -aE "ccrun|cc500:|exec|byte-identical|out2.elf|good2|writefile" "$LOG" | tail -30
exit 0
