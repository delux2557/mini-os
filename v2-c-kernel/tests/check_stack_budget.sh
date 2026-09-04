#!/usr/bin/env bash
# SEC-07/L0 关键根链栈预算门禁（CI 回归）。
#
# 背景：mini-os 所有中断/异常/syscall 走中断门（进入即关 IF），故 4KB 内核栈
# （KSTACK_SIZE，sched.c TSS esp0 = kstack_top）同一时刻只承载"单条处理链"：
# IRQ 链 或 syscall 链，无嵌套叠加。SEC-07（DHCP 续约链 5528B 静默写穿）教训：
# 逐层开着整帧/整报缓冲会击穿 4KB 栈。L0 已加页底 canary 兜底（可诊断停机）；
# 本门禁是"静态防线"——用 gcc -fstack-usage 产出的 .su 逐帧测量，对每条已登记
# 关键根链断言：每帧 ≤ 单帧上限、整链总和 ≤ 预算（KSTACK − 512 裕量）。
#
# 单链独占栈 ⇒ 链上所有帧同时存活，用"总和"建模（恰契运行期峰值）；任何一环回退
# （新网卡驱动、新轮询、深 syscall 引入大帧）立即红。
#
# 用法：check_stack_budget.sh <.su目录> [链总和预算, 默认 3584]
# 依赖：make target `test-stack` 先以 -fstack-usage 编译产出 .su。

set -u
SUDIR="${1:?usage: $0 <sudir> [budget]}"
BUDGET="${2:-3584}"          # = KSTACK_SIZE(4096) − 512 裕量
KSTACK=4096                  # src/kernel/sched.c KSTACK_SIZE（防空预算越界误设）

# 命中即退出
fail() { echo "[FAIL] $*" >&2; exit 1; }

# 单函数帧上限：任一帧也不得逼近整栈（防单个函数回归直接吞栈）
SINGLE=$(( KSTACK * 3 / 4 ))   # 3072

# .su 可能存在 == 0 的"无需帧"项（小函数）；不存在条目也要报出（防止改名漏测）。
# .su 行格式：<file>:<line>:<col>:<func>[.constprop.N]\t<frames>\t<attr>
# gcc 可能对每个函数生成 .constprop/.isra 特化克隆，故按基名聚合，取"全部克隆中
# 最大帧"（= 最坏实际路径，向上收敛，作门禁是安全的）。
stack_of() {
    local want="$1"
    awk -v w="$want" '
        { fn=$1; sub(/^.*:/,"",fn); sub(/\..*$/,"",fn);
          if (fn==w) { n=$2+0; if (n>best) best=n } }
        END { print (best+0) }' "$SUDIR"/*.su
}

# 已登记关键根链：`入口` 是该链在内核栈上的第一帧（IRQ handler 或 syscall_dispatch），
# 其后是按调用深度可达的最深线性路径。任一新根链都须在此登记（门禁才会守护它——
# L2 将自动化，扫描 cgraph 自动求全，此处为过渡期的人工清单）。
ROOTS=(
  "IRQ0/timer·DHCP续约: timer_cb e1000_dhcp_tick dhcp_poll_once netsock_dhcp_recv netsock_drain netif_rx e1000_if_rx"
  "IRQ0/timer·调度: timer_cb sched_tick schedule"
  "IRQ1/键盘: kb_cb sched_wake_keyboard kb_line_take"
  "IRQ4/串口: serial_irq"
  "syscall/recvfrom: syscall_dispatch netsock_recv netsock_drain netif_rx e1000_if_rx"
  "syscall/sendto: syscall_dispatch sys_sendto_case netsock_send"
  "syscall/exec: syscall_dispatch sys_exec_case sched_exec load_elf_file"
  "syscall/fork: syscall_dispatch sched_fork"
  "syscall/ls: syscall_dispatch sys_fs_ls_case"
)

[ "$BUDGET" -lt "$KSTACK" ] || fail "预算 ${BUDGET}B 不得 ≥ KSTACK ${KSTACK}B（失去防越界意义）"

for entry in "${ROOTS[@]}"; do
    root="${entry%%:*}"; chain="${entry#*: }"
    echo "[stack] 链 ■$root■（栈 ${KSTACK}B，预算 ${BUDGET}B）："
    sum=0
    for fn in $chain; do
        s=$(stack_of "$fn")
        if [ -z "$s" ] || [ "$s" -eq 0 ] 2>/dev/null; then
            fail "函数 $fn 在 .su 未找到（可能更名/被内联导致漏测，或 -fstack-usage 未生效）"
        fi
        printf '  %-22s %6dB\n' "$fn" "$s"
        [ "$s" -le "$SINGLE" ] || fail "$fn 单帧 ${s}B 超单帧上限 ${SINGLE}B"
        sum=$(( sum + s ))
    done
    echo "[stack]   $root 链总和 ${sum}B / 预算 ${BUDGET}B"
    [ "$sum" -le "$BUDGET" ] || fail "$root 链栈总和 ${sum}B 超预算 ${BUDGET}B => 4KB 内核栈写穿风险（SEC-07）"
done
echo "[stack] OK（全部关键根链在 ${BUDGET}B 预算内）"