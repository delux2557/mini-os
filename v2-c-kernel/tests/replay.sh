#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/replay.sh
# record/replay 地基 · P3 回放器（transcript 消费侧）
#
# 消费 P2 录制的 `*.in.tr`（序号/相对ms/命令），按相对时间关系重放到 QEMU 串口，
# 产出 `*.out.tr` 原始字节。与黄金 transcript 或另一轮回放做**确定性差分**，
# 形成"录旧版失败 → 修复版回放 → 输出不一致"的闭环前提。
#
# 用法（source 后调用）：
#   . "$(dirname "$0")/replay.sh"
#   replay_into <in.tr> <out.log> [runid]     # 重放一轮，串口输出进 <out.log>
#   然后 tr_snapshot <out.log> / cmp 与黄金差分 由调用方/外层脚本驱动
#
# 环境变量：REPLAY_ICOUNT=1 用 P1 icount 确定性时钟（同输入同输出，逐字节稳定）
set -u

# F1 输入背压：回放以"上一行已被 guest readline 消费"为节拍钳制，不再只按 rel_ms sleep——杜绝
# 多行字节淤积跨行合并/吞行。rel_ms 仅留审计。消费信号两条路径（详见 transcript.sh TR_ACK_RE）：
#   行到达时读方已阻塞 -> `[sched] wake keyboard waiter pid=.. (N bytes)`（串口驱动常态）
#   行到达前缓冲已就绪 -> `[kb] readline pid=.. -> N bytes`
RP_ACK_RE='\[sched\] wake keyboard waiter pid=[0-9]+ \([0-9]+ bytes\)|\[kb\] readline pid=[0-9]+ -> [0-9]+ bytes'
RP_ACK_TIMEOUT="${RP_ACK_TIMEOUT:-30}"

# 统计 out_log 中已消费的行级 readline ack 条数
rp_ack_count() {
    local c
    c="$(grep -acE "$RP_ACK_RE" "$1" 2>/dev/null || true)"
    echo "${c:-0}"
}

# 等到 out_log 的 ack 计数达到 <need>；<sec> 秒内未达返回 1（由调用方告警后继续）
rp_ack_wait() {
    local log="$1" need="$2" sec="${3:-$RP_ACK_TIMEOUT}" t=0
    while [ "$t" -lt $((sec * 2)) ]; do
        [ "$(rp_ack_count "$log")" -ge "$need" ] && return 0
        sleep 0.5; t=$((t + 1))
    done
    return 1
}

# 重放一轮 transcript 输入流到 QEMU 串口，输出进 <out.log>
# replay_into <in.tr> <out.log> [<runid>] [<done_regex>]
#   <done_regex>：末条命令的完成信号（扩展正则，含之即算本轮结束）。缺省=继续等 shell 提示符
#   后即收（适合纯命令流）。**不用"日志静止"判据**：本内核有后台 demo 应用(pld/net recvfrom)
#   持续打印，日志永不静止；以"信号出现"为准更准确与快。
replay_into() {
    local in_tr="$1" out_log="$2"
    local runid="${3:-replay}" done_re="${4:-mini-os\$ }"
    local icount_flag=""
    local TIN="build/${runid}_in.fifo" TOUT="build/${runid}_out.fifo" QPID="" CAT_PID=""
    [ "${REPLAY_ICOUNT:-0}" = "1" ] && icount_flag="-icount shift=auto,align=on,sleep=on"

    rm -f "$out_log" "$TIN" "$TOUT"; mkfifo "$TIN" "$TOUT"
    (cat "$TOUT" > "$out_log") & CAT_PID=$!
    qemu-system-i386 -kernel build/kernel.elf -display none -vga std -no-reboot -no-shutdown \
        -m 64 -nic none -serial stdio -monitor none $icount_flag < "$TIN" > "$TOUT" 2>/dev/null &
    QPID=$!
    exec 9>"$TIN"

    # 等 shell 提示符就位（重放第 1 条命令的前置；之后按 in.tr 相对时间打点）。
    # icount 下内核推进慢（启动期 DHCP/e1000 等耗时 host 墙钟远超虚拟时间），故首条命令
    # 必须以"提示符就绪"为硬下限，而非按 in.tr 第一个 rel_ms 打拍——否则会被吞进启动期。
    local i
    for ((i=0;i<300;i++)); do grep -aq 'mini-os\$ ' "$out_log" 2>/dev/null && break; sleep 0.2; done
    grep -aq 'mini-os\$ ' "$out_log" 2>/dev/null || { echo "[replay] 超时：未等到 shell 提示符"; }

    # 逐行消费 in.tr：seq \t rel_ms \t payload。不必等 rel_ms delta——以"上一条已被 guest
    # readline 消费"为真实节拍钳制（F1 输入背压），rel_ms 仅留审计。
    local seq rel payload base acked
    base="$(rp_ack_count "$out_log")"; acked=0
    while IFS=$'\t' read -r seq rel payload; do
        case "${seq:-}" in ''|'#'*) continue ;; esac   # 跳过 header/空/注释行
        # 注入本条前，先等"此前已注入的各行全被 guest 消费"（ack 计数 >= base+acked）。
        # 若超时：告警但继续，避免慢 icount 一轮卡死（此前的 ack 已保证无跨行合并）。
        if ! rp_ack_wait "$out_log" $((base + acked)); then
            echo "[replay] 警告：注入 '$payload' 前，前 ${acked} 条本应有的 readline ack 未齐（${RP_ACK_TIMEOUT}s 超时）——可能吞行" >&2
        fi
        printf '%s\n' "$payload" >&9
        acked=$((acked + 1))
    done < "$in_tr"

    # 等完成信号（done_re）出现；超时 180s 上限（icount 下编译/命令可很慢）。
    local wait=0
    while [ "$wait" -lt 360 ]; do
        grep -aqE "$done_re" "$out_log" 2>/dev/null && break
        sleep 0.5; wait=$(( wait + 1 ))
    done
    exec 9>&- 2>/dev/null || true
    kill "$QPID" 2>/dev/null || true; wait "$CAT_PID" 2>/dev/null || true
    rm -f "$TIN" "$TOUT"
}