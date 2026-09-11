/* mini-os/v2-c-kernel/src/app/dhcpd.c
 * R1.3（TD-05 前半，外部审计 A1）：DHCP 租期续约守护进程。
 * 续约状态机仍在内核（e1000_dhcp_tick，非阻塞：每 tick 至多"发一帧/收一帧"），
 * 但执行上下文从 timer 中断（timer_cb/IRQ0 ISR）迁到本守护进程——每 10ms
 * 醒来经 syscall#39 触发一次。消除"续约策略在中断上下文运行"的寄生：
 *   - 续约不再挤占中断栈/ISR 预算；被抢占也不丢 tick（下次醒来补上）
 *   - 无租约/无网卡时内核侧直接 no-op，本进程仅是 10ms 心跳
 * 由 kernel.c 在网卡就绪时 spawn（同 sockdemo 模式）。 */
#include "user_lib.h"

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (;;) {
        sys_sleep(1);                       /* 100Hz 心跳 -> 每 10ms 醒来一次 */
        syscall3(SYS_DHCP_TICK, 0, 0, 0);
    }
}
