#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_persist.sh
# v0.16 ATA 真盘持久化回归：两次 QEMU 运行共享同一磁盘镜像。
#   第 1 次：格式化空白盘 -> mkdir /persist -> save 写回 -> exit
#   第 2 次：挂载同一镜像重启 -> ls / 应见 persist/（用户数据跨重启存活）
set -u
cd "$(dirname "$0")/.." || exit 1

IMG="build/persist.img"
TIN="build/persist_in.fifo"
TOUT="build/persist_out.fifo"
FAIL=0
QPID=""; CAT_PID=""

cleanup() {
    exec 9>&- 2>/dev/null || true
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true
    rm -f "$TIN" "$TOUT"
}
trap cleanup EXIT

wait_for() {   # wait_for <日志> <说明> <正则> [超时秒]
    local log="$1" desc="$2" re="$3" tmo="${4:-8}" i
    for ((i = 0; i < tmo * 4; i++)); do
        grep -aq "$re" "$log" 2>/dev/null && { echo "[ok]   $desc"; return 0; }
        sleep 0.25
    done
    echo "[FAIL] $desc (缺: $re)"
    FAIL=$((FAIL + 1)); return 1
}

boot() {   # boot <日志>：启动 QEMU（-hda 共享磁盘镜像），返回后即可发命令
    local log="$1"
    rm -f "$log" "$TIN" "$TOUT"
    mkfifo "$TIN" "$TOUT"
    (cat "$TOUT" > "$log") & CAT_PID=$!
    qemu-system-i386 -kernel build/kernel.elf -hda "$IMG" -display none -vga std \
        -no-reboot -no-shutdown -m 64 -serial stdio -monitor none \
        < "$TIN" > "$TOUT" 2>/dev/null &
    QPID=$!
    exec 9>"$TIN"
}

shutdown() {
    exec 9>&- 2>/dev/null || true
    kill "$QPID" 2>/dev/null || true; QPID=""
    wait "$CAT_PID" 2>/dev/null || true; CAT_PID=""
    rm -f "$TIN" "$TOUT"
}

send() { printf '%s\n' "$1" >&9; sleep 0.3; }

echo "== [1/4] 构建内核 =="
make >/dev/null 2>&1 || { echo "[FAIL] 内核构建失败"; exit 1; }
echo "      构建完成"

echo "== [2/4] 生成 1MB 空白磁盘镜像 =="
dd if=/dev/zero of="$IMG" bs=1024 count=1024 2>/dev/null
echo "      镜像: $(stat -c%s "$IMG") bytes"

echo "== [3/4] 第 1 次运行：格式化 + 建目录 + save =="
LOG1="build/persist1.log"
boot "$LOG1"
wait_for "$LOG1" "shell 提示符"        "mini-os\$ " 20
wait_for "$LOG1" "ATA 探测到磁盘"      "\[ata\] IDE disk:"
wait_for "$LOG1" "空白盘格式化"        "disk blank -> format"
send "mkdir /persist"
wait_for "$LOG1" "mkdir 建 /persist"   "\[shell\] mkdir '/persist' -> "
send "save"
wait_for "$LOG1" "save 写回磁盘"       "\[shell\] save -> 0"
wait_for "$LOG1" "storage 保存日志"    "\[storage\] saved "
send "exit"
wait_for "$LOG1" "shell 退出"          "bye"
shutdown

echo "== [4/4] 第 2 次运行：重启挂载同一镜像，校验数据仍在 =="
LOG2="build/persist2.log"
boot "$LOG2"
wait_for "$LOG2" "重启挂载持久盘"      "persistent FS mounted"
wait_for "$LOG2" "shell 提示符"        "mini-os\$ " 20
send "ls"
wait_for "$LOG2" "重启后 /persist 仍在" "\[ls\]   persist/ "
send "selftest"
wait_for "$LOG2" "持久盘应用可运行"    "\[selftest\] PASS (5 checks)" 20
shutdown

echo
if [ "$FAIL" -eq 0 ]; then
    echo "ATA 持久化回归通过（用户数据跨重启存活）"
    exit 0
else
    echo "ATA 持久化回归失败: $FAIL 项未通过"
    exit 1
fi
