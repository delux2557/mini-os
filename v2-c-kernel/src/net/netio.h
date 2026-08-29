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

#endif /* NET_NETIO_H */
