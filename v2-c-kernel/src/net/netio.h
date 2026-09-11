/* mini-os/v2-c-kernel/src/net/netio.h
 * 用户态网络 I/O 参数结构（v0.20）：内核(usermode.c)与用户(user_lib.h)共用，
 * 保证 ABI 一致。3 参数 syscall 约定下用"用户内存中的结构体"承载多参
 * （iocb/msghdr 风格），避免扩 syscall 寄存器数。
 * 布局以 _pad 显式对齐：sizeof == 16，便于 copyin/copyout 整块搬运。 */
#ifndef NET_NETIO_H
#define NET_NETIO_H
#include <stdint.h>

/* sys_net_sendto：向 dst_ip:dst_port 发送载荷（buf/len 位于用户内存） */
struct net_send_iov {
    uint32_t      dst_ip;      /* 主机字节序，如 10.0.2.2 = 0x0A000202 */
    uint16_t      dst_port;
    uint16_t      _pad;
    const uint8_t *buf;        /* 载荷（用户内存） */
    uint32_t      len;
};

/* sys_net_recvfrom：取队首数据报；buf/max 为载荷缓冲，src_* 为出参 */
struct net_recv_iov {
    uint32_t src_ip;           /* 出参：源 IP */
    uint16_t src_port;         /* 出参：源端口 */
    uint16_t _pad;
    uint8_t  *buf;             /* 载荷缓冲（用户内存） */
    uint32_t max;
};

/* ---- v0.38（R1.3 后半，外部审计 A1）：DHCP 续约原子能力 ABI ----
 * 续约状态机（RFC 2131 §4.4.5）整体移至用户态 dhcpclient：内核只暴露
 * 机制（查询租约 / 发一帧 / 收一帧 / 应用 ACK / 回退静态），策略（何时
 * 续约、超时升级、重新获取）由用户态进程决策——"策略寄生内核"根除。
 * 内核 dhcp_send_* / dhcp_poll_once / dhcp_apply_ack 的日志保留（供测试断言）。 */

/* sys_dhcp_query()：查询租约状态（出参，内核 copyout 填充） */
struct dhcp_query_iov {
    uint32_t lease_secs;       /* 租期秒数；0 = 未取得租约（静态兜底，无需续约） */
    uint32_t elapsed;          /* 距最近一次 ACK 的 tick 数 */
    uint32_t t1_ticks;         /* T1 阈值（相对 acquired，0.5×lease） */
    uint32_t t2_ticks;         /* T2 阈值（相对 acquired，0.875×lease） */
};

/* sys_dhcp_recv()：取一条 DHCP 应答并解析（出参；返回 1=收到 / 0=无包 / -1=失败） */
struct dhcp_reply_iov {
    uint32_t mt;               /* 消息类型：2=OFFER 5=ACK 6=NAK */
    uint32_t yi;               /* yiaddr（分配 IP） */
    uint32_t si;               /* server ip */
    uint32_t rt;               /* router（网关） */
    uint32_t ls;               /* 租期（秒） */
};

/* sys_dhcp_apply()：把 ACK 解析结果应用回内核租约（tag 决定内核日志措辞） */
struct dhcp_apply_iov {
    uint32_t yi;               /* 新本机 IP */
    uint32_t rt;               /* 新网关 IP */
    uint32_t ls;               /* 新租期 */
    uint32_t tag;              /* 0=renew 1=rebind 2=re-acquire（仅日志措辞） */
};

#endif /* NET_NETIO_H */
