/* mini-os/v2-c-kernel/src/app/dhcpd.c
 * R1.3 后半（TD-05 收尾，外部审计 A1）：DHCP 租期续约状态机——全用户态。
 * 续约决策（RFC 2131 §4.4.5：T1 单播 RENEW -> T2 广播 REBIND -> 超时重新获取）
 * 由本守护进程实现，内核只暴露原子能力（syscall#40-44：查询租约/发一帧/收一帧/
 * 应用 ACK/回退静态）——"策略寄生内核"根除，内核不再承载续约策略。
 * 每 10ms 醒来驱动一次状态机（非阻塞：每次至多"发一帧/收一帧"）。
 * 无租约（静态兜底）时查询返回 lease=0，状态机整体 no-op。 */
#include "user_lib.h"

/* 状态：对应 RFC 2131 §4.4.5（内核原 e1000_dhcp_tick 的迁移版） */
enum { RENEW_NONE = 0, RENEW_SENT, REBIND_SENT, REACQ_OFFER, REACQ_ACK };

/* 经 syscall#40-44 驱动内核 DHCP 原子能力 */
static int  dhcp_query(struct dhcp_query_iov *q) { return (int)syscall3(SYS_DHCP_QUERY, (uint32_t)q, 0, 0); }
static int  dhcp_send(uint32_t type, uint32_t req_ip) { return (int)syscall3(SYS_DHCP_SEND, type, req_ip, 0); }
static int  dhcp_recv(struct dhcp_reply_iov *r) { return (int)syscall3(SYS_DHCP_RECV, (uint32_t)r, 0, 0); }
static int  dhcp_apply(uint32_t yi, uint32_t rt, uint32_t ls, uint32_t tag) {
    struct dhcp_apply_iov ap;
    ap.yi = yi; ap.rt = rt; ap.ls = ls; ap.tag = tag;
    return (int)syscall3(SYS_DHCP_APPLY, (uint32_t)&ap, 0, 0);
}
static void dhcp_fallback(void) { syscall3(SYS_DHCP_FALLBACK, 0, 0, 0); }

static void client_log(const char *msg) {
    sys_print("[dhcpclient] ");
    sys_print(msg);
    sys_print("\n");
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int state = RENEW_NONE;
    uint32_t state_tick = 0;

    for (;;) {
        sys_sleep(1);                       /* 100Hz 心跳 -> 每 10ms 驱动一次 */

        struct dhcp_query_iov q;
        if (dhcp_query(&q) != 0) continue;  /* 查询失败：下轮再试 */
        if (q.lease_secs == 0) {            /* 无租约（静态兜底/未取址）：整体 no-op */
            state = RENEW_NONE;
            continue;
        }
        uint32_t now = sys_getticks();
        uint32_t el = q.elapsed;

        switch (state) {
        case RENEW_NONE:                    /* 已取得租约，等 T1；过 T2 直接 REBIND */
            if (el >= q.t2_ticks) {
                dhcp_send(1, 0);            /* REBIND */
                state = REBIND_SENT; state_tick = now;
            } else if (el >= q.t1_ticks) {
                dhcp_send(0, 0);            /* RENEW */
                state = RENEW_SENT; state_tick = now;
            }
            break;

        case RENEW_SENT: {                  /* T1 已发单播 RENEW，等 ACK；到 T2 升 REBIND */
            struct dhcp_reply_iov r;
            int n = dhcp_recv(&r);
            if (n == 1) {
                if (r.mt == 5) { dhcp_apply(r.yi, r.rt, r.ls, 0); state = RENEW_NONE; }
                else if (r.mt == 6) { client_log("RENEW NAK -> re-acquire");
                                      dhcp_send(2, 0); state = REACQ_OFFER; state_tick = now; }
            } else if (el >= q.t2_ticks) {
                dhcp_send(1, 0);            /* T1 后 T2 到仍未 ACK：升 REBIND */
                state = REBIND_SENT; state_tick = now;
            }
            break;
        }

        case REBIND_SENT: {                 /* T2 已发广播 REBIND，等 ACK；超时重新获取 */
            struct dhcp_reply_iov r;
            int n = dhcp_recv(&r);
            if (n == 1) {
                if (r.mt == 5) { dhcp_apply(r.yi, r.rt, r.ls, 1); state = RENEW_NONE; }
                else if (r.mt == 6) { client_log("REBIND NAK -> re-acquire");
                                      dhcp_send(2, 0); state = REACQ_OFFER; state_tick = now; }
            } else if ((uint32_t)(now - state_tick) > (q.t2_ticks - q.t1_ticks)) {
                client_log("REBIND timeout -> re-acquire");
                dhcp_send(2, 0);            /* DISCOVER */
                state = REACQ_OFFER; state_tick = now;
            }
            break;
        }

        case REACQ_OFFER: {                 /* 重新获取：等 OFFER -> 发 REQUEST */
            struct dhcp_reply_iov r;
            int n = dhcp_recv(&r);
            if (n == 1 && r.mt == 2) {
                dhcp_send(3, r.yi);         /* REQUEST(offer 的 IP) */
                state = REACQ_ACK; state_tick = now;
            } else if ((uint32_t)(now - state_tick) > 200) {   /* 2s 超时 */
                client_log("re-acquire fail -> static fallback");
                dhcp_fallback();
                state = RENEW_NONE;
            }
            break;
        }

        case REACQ_ACK: {                   /* 等 ACK；NAK 则 rediscover */
            struct dhcp_reply_iov r;
            int n = dhcp_recv(&r);
            if (n == 1) {
                if (r.mt == 5) { dhcp_apply(r.yi, r.rt, r.ls, 2); state = RENEW_NONE; }
                else if (r.mt == 6) { client_log("re-acquire NAK -> rediscover");
                                      dhcp_send(2, 0); state = REACQ_OFFER; state_tick = now; }
            } else if ((uint32_t)(now - state_tick) > 200) {   /* 2s 超时 */
                client_log("re-acquire fail -> static fallback");
                dhcp_fallback();
                state = RENEW_NONE;
            }
            break;
        }
        }
    }
}
