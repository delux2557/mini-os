#!/usr/bin/env bash
# SEC-07 内核栈预算门禁（CI 回归）。
#
# 背景：DHCP 租期续约链由 timer_cb 在 IRQ0 中断上下文驱动，运行在当前进程的
# 4KB 内核栈（TSS esp0 = kstack_top，见 sched.c KSTACK_SIZE）上。修复前该链在
# 逐层开着 ~1.6KB ~ 2KB 的整帧/整报缓冲（dhcp_poll_once bootp[2048] +
# netsock_drain f[1600] + e1000_if_rx eth[1600]），栈帧总和 5528B > 4096B，
# 静默写穿内核栈（SEC-07）。本门禁用 gcc -fstack-usage 产出的 .su 逐帧测量，
# 断言链上每个函数帧 ≤ 单帧上限、且整链总和 ≤ 预算，任何一环回退立即红。
#
# 用法：check_stack_budget.sh <.su目录> [链总和预算, 默认 3584]
# 依赖：make target `test-stack` 先以 -fstack-usage 编译产出 .su。

set -u
SUDIR="${1:?usage: $0 <sudir> [budget]}"
BUDGET="${2:-3584}"          # = KSTACK_SIZE(4096) − 512 裕量
KSTACK=4096                  # src/kernel/sched.c KSTACK_SIZE（防空预算越界误设）

# 命中即退出
fail() { echo "[FAIL] $*" >&2; exit 1; }

# 链上关键函数（按调用深度）；gcc 可能对每个函数生成 .constprop/.isra 特化克隆，
# 故按基名聚合，取"全部克隆中最大帧"（= 最坏实际路径，向上收敛，作门禁是安全的）。
CHAIN=(e1000_dhcp_tick dhcp_poll_once netsock_dhcp_recv netsock_drain netif_rx e1000_if_rx)

# 单函数帧上限：任一帧也不得逼近整栈（防单个函数回归直接吞栈）
SINGLE=$(( KSTACK * 3 / 4 ))   # 3072

# .su 可能存在 == 0 的"无需帧"项（小函数）；不存在条目也要报出（防止改名漏测）。
# .su 行格式：<file>:<line>:<col>:<func>[.constprop.N]\t<frames>\t<attr>
stack_of() {
    local want="$1"
    awk -v w="$want" '
        { fn=$1; sub(/^.*:/,"",fn); sub(/\..*$/,"",fn);
          if (fn==w) { n=$2+0; if (n>best) best=n } }
        END { print (best+0) }' "$SUDIR"/*.su
}

sum=0
echo "[stack] SEC-07 链（IRQ0 中断栈 ${KSTACK}B，预算 ${BUDGET}B）："
for fn in "${CHAIN[@]}"; do
    s=$(stack_of "$fn")
    if [ -z "$s" ] || [ "$s" -eq 0 ] 2>/dev/null; then
        fail "函数 $fn 在 .su 未找到（可能更名/被内联导致漏测，或 -fstack-usage 未生效）"
    fi
    printf '  %-22s %6dB\n' "$fn" "$s"
    [ "$s" -le "$SINGLE" ] || fail "$fn 单帧 ${s}B 超单帧上限 ${SINGLE}B"
    sum=$(( sum + s ))
done
echo "[stack] 链总和 ${sum}B / 预算 ${BUDGET}B"
[ "$BUDGET" -lt "$KSTACK" ] || fail "预算 ${BUDGET}B 不得 ≥ KSTACK ${KSTACK}B（失去防越界意义）"
[ "$sum" -le "$BUDGET" ] || fail "链栈总和 ${sum}B 超预算 ${BUDGET}B => 4KB 内核栈写穿风险（SEC-07）"
echo "[stack] OK（SEC-07 链预算内）"