#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_determinism.sh
# record/replay 地基 · P1 验收（确定性启动）
#
# 两次 QEMU 冷启动（-icount 确定性时钟），比对串口日志逐字节一致——
# 证明"同输入同输出"：定时器/中断/调度/网络握手在同 icount 虚拟时钟下完全确定。
# 这是 transcript 固化/回放差分（P2/P3）的前置地基。
#
# 依赖：qemu-system-i386。净态 host 墙钟（sleep 冷启）仅用于"推进到后再 diff"，不参与断言。
set -u
cd "$(dirname "$0")/.." || exit 1
for c in qemu-system-i386; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

ICOUNT="-icount shift=auto,align=on,sleep=on"
IC="${ICOUNT:-}"
DURATION="${DURATION:-20}"     # 冷启动时长（默认 20s；IC 模式无 KVM 时推进偏慢可调大）
LA="build/det_a.log"; LB="build/det_b.log"

echo "== [1/2] 内核构建 =="
make >/dev/null 2>&1 || { echo "[FAIL] 内核构建失败"; exit 1; }
echo "      构建完成"

echo "== [2/2] 两次 icount 冷启动，逐字节 diff 串口日志 =="
rm -f "$LA" "$LB"
timeout "$DURATION" qemu-system-i386 -kernel build/kernel.elf -display none \
    -serial file:"$LA" -no-reboot -no-shutdown -m 64 $IC >/dev/null 2>&1 &
PA=$!
timeout "$DURATION" qemu-system-i386 -kernel build/kernel.elf -display none \
    -serial file:"$LB" -no-reboot -no-shutdown -m 64 $IC >/dev/null 2>&1 &
PB=$!
wait "$PA" "$PB" 2>/dev/null

N_A=$(wc -l < "$LA" 2>/dev/null || echo 0)
N_B=$(wc -l < "$LB" 2>/dev/null || echo 0)
echo "      日志 A=${N_A} 行, B=${N_B} 行"

if [ "$N_A" -eq 0 ]; then echo "[FAIL] 内核未生成串口日志（icount 下可能推进过慢）"; exit 1; fi

if cmp -s "$LA" "$LB"; then
    echo "[ok]   icount 冷启动串口日志逐字节一致（${N_A} 行）——内核确定性成立"
    echo "[PASS] 确定性启动验收通过"
    exit 0
else
    echo "[FAIL] 两次 icount 冷启动输出不一致——存在 host 墙钟/非确定依赖"
    diff "$LA" "$LB" | head -20
    exit 1
fi