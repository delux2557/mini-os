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

    # 逐行消费 in.tr：seq \t rel_ms \t payload；按相邻 rel_ms 的增量 sleep 打拍
    local seq prev rel payload delta
    prev=-1
    while IFS=$'\t' read -r seq rel payload; do
        case "${seq:-}" in ''|'#'*) continue ;; esac   # 跳过 header/空/注释行
        if [ "$prev" -ge 0 ] && [ "$rel" -ge "$prev" ]; then
            delta=$(( rel - prev ))
            # 上限 5s：防异常大 delta 拖长回放（对应命令本身须由内核处理，见边界说明）
            [ "$delta" -gt 5000 ] && delta=5000
            sleep $(( delta / 1000 )).$(( (delta % 1000) / 100 ))
        fi
        prev="$rel"
        printf '%s\n' "$payload" >&9
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