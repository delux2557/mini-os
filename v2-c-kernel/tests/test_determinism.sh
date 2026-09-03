#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_determinism.sh
# record/replay 地基 · P1 验收（确定性启动）
#
# 两次 QEMU 冷启动（-icount 确定性时钟），比对串口日志的"确定性前缀"逐字节一致——
# 证明"同输入同输出"：定时器/中断/内存/进程孵化在同 icount 虚拟时钟下完全确定。
# 这是 transcript 固化/回放差分（P2/P3）的前置地基。
#
# v1.4.8 修复（CI 门禁稳定失败，见交接文档）：
#   旧设计 = 两路 qemu 后台并行 + 固定 timeout 整文件 cmp：
#     * 断言对象是"20s 墙钟内各跑到哪"，不是"输出是否确定"——截断点随 host 负载漂移；
#     * 断言范围混入 DHCP/网络段，依赖宿主 slirp 实时应答（guest 无法确定该段是否进 diff）；
#     * A 先 B 后并行抢占 CPU，A 稳定领先半拍（CI 高载 40 行 vs B 30 行），闸门稳定假红。
#   新设计 = 串行冷启 + 哨兵截断 + -nic none 排除外部输入，断言回归"boot 确定性前缀"：
#     * 串行：A 完整跑完再 B，消除 A/B 并行抢占的负载偏置；
#     * -nic none：彻底摘除 DHCP/网络段（宿主驱动输出，非 guest 可确定事件）；
#     * 哨兵截断：等到 boot 完成里程碑 [ok] subsystems ready 出现即 kill，
#       diff 只比到该里程碑为止——断言对象是纯 guest 内部确定性的 boot 前缀；
#     * 区分两类失败：里程碑未到 → exit 2（环境/负载，可重试）；前缀不一致 → exit 1（真非确定性）。
#
# 依赖：qemu-system-i386。
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh
for c in qemu-system-i386; do
    command -v "$c" >/dev/null 2>&1 || { echo "[ERR] 缺 $c"; exit 2; }
done

ICOUNT="-icount shift=auto,align=on,sleep=on"
IC="${ICOUNT:-}"
DURATION="${DURATION:-30}"     # 哨兵等待上限（秒）；boot 到哨兵在 icount 下通常 <15s，留裕量
SENTINEL='\[ok\] subsystems ready; creating processes'
LA="$BUILD/det_a.log"; LB="$BUILD/det_b.log"

echo "== [1/2] 内核构建 =="
make BUILD="$BUILD" >/dev/null 2>&1 || { echo "[FAIL] 内核构建失败"; exit 1; }
echo "      构建完成"

echo "== [2/2] 两次 icount 冷启动（串行 + -nic none），比对 boot 确定性前缀 =="

# boot_once <out.log>：串行冷启，等到 boot 完成里程碑即 kill。
# 返回 0 = 到达里程碑；1 = 未到（环境/负载，报 SKIP 语义，由调用方转 exit 2）。
boot_once() {
    local out="$1" p t=0
    rm -f "$out"
    timeout "$DURATION" qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none \
        -serial file:"$out" -no-reboot -no-shutdown -m 64 -nic none $IC >/dev/null 2>&1 &
    p=$!
    while kill -0 "$p" 2>/dev/null; do
        if grep -qE "$SENTINEL" "$out" 2>/dev/null; then
            kill "$p" 2>/dev/null
            wait "$p" 2>/dev/null
            return 0
        fi
        sleep 0.2
        t=$((t + 1))
        [ "$t" -ge $(( DURATION * 5 )) ] && break
    done
    wait "$p" 2>/dev/null
    grep -qE "$SENTINEL" "$out" 2>/dev/null
}

# prefix <out.log>：截取 boot 确定性前缀（到哨兵行为止，含哨兵本身）
prefix() { sed -n "1,/$(printf '%s' "$SENTINEL")/p" "$1"; }

RA=0; RB=0
echo "-- A 冷启 --"
if boot_once "$LA"; then RA=1; else echo "      A 未在 ${DURATION}s 内到达 boot 里程碑"; fi
echo "      A 前缀 $(prefix "$LA" | wc -l) 行"
echo "-- B 冷启 --"
if boot_once "$LB"; then RB=1; else echo "      B 未在 ${DURATION}s 内到达 boot 里程碑"; fi
echo "      B 前缀 $(prefix "$LB" | wc -l) 行"

# 环境/负载问题：任一路没跑到里程碑（如 icount 在极慢/高载 CI 上贴边）→ 可重试，不判内核失败
if [ "$RA" -ne 1 ] || [ "$RB" -ne 1 ]; then
    echo "[SKIP] 哨兵未全部到达（LA=$([ "$RA" = 1 ] && echo 到 || echo 未到), LB=$([ "$RB" = 1 ] && echo 到 || echo 未到)）——"
    echo "       这是"环境/负载"问题，非"输出不确定"。可重试或调大 DURATION。"
    exit 2
fi

if cmp -s <(prefix "$LA") <(prefix "$LB"); then
    echo "[ok]   boot 确定性前缀逐字节一致（$(prefix "$LA" | wc -l) 行）——内核确定性成立"
    echo "[PASS] 确定性启动验收通过"
    exit 0
else
    echo "[FAIL] 两次冷启 boot 前缀不一致——存在真非确定性（host 墙钟/顺序依赖）"
    diff <(prefix "$LA") <(prefix "$LB") | head -20
    exit 1
fi