/* mini-os/v2-c-kernel/src/net/tcp_proto.h
 * 虚拟 TCP 会话协议（netif Step 4）——wire 常量 + 纯逻辑会话头解析。
 * 单一事实来源：
 *   - 用户侧薄包装（src/app/tcp.c）按此组帧/解帧
 *   - 宿主转发器（tests/tcp_proxy.py）按镜像常量解析（Python 侧）
 *   - 宿主 fuzz（tests/fuzz_parse.c case 7）复用本函数做畸形头模糊
 * 语义以 docs/tcp-session-proto.md 为准（版本 v1.1，§2.1 含控制类消息）。
 * 头布局（8 字节定长，大端）：[session_id(4) | msg_type(1) | version(1) | flags(2)]。
 */
#ifndef NET_TCP_PROTO_H
#define NET_TCP_PROTO_H
#include <stdint.h>

#define TCP_PHDR     8          /* 会话协议头定长 */

/* msg_type 取值（docs v1.1 §2.1：0x02-0x05 事件 host→guest，0x06-0x07 控制 guest→host） */
#define MSG_DATA     0x01       /* 应用数据（双向；host→guest 的 flags 低 16 位携带数据序列号 seq） */
#define MSG_OPENED   0x02       /* 事件：TCP 连接建立成功 */
#define MSG_CLOSED   0x03       /* 事件：对端正常关闭 */
#define MSG_ERROR    0x04       /* 事件：连接失败 / 对端拒绝 */
#define MSG_TIMEOUT  0x05       /* 事件：打开/发送超时、半开清理 */
#define MSG_OPEN     0x06       /* 控制：连接请求（payload= dst_ip(4BE)+dst_port(2BE)） */
#define MSG_CLOSE    0x07       /* 控制：注销请求（payload 空） */
#define MSG_ACK      0x08       /* 控制（guest→host，v1.2 可靠下行）：累计 ACK，
                                   payload= guest 下一个期望 seq（2BE，high then low） */

#define TCP_PROTO_VERSION 0x01  /* version 字段 = 1 */

static inline void tcp_hdr_put_session(uint8_t *p, uint32_t session_id) {
    p[0] = (uint8_t)(session_id >> 24);
    p[1] = (uint8_t)(session_id >> 16);
    p[2] = (uint8_t)(session_id >> 8);
    p[3] = (uint8_t)session_id;
}

static inline void tcp_hdr_set_type(uint8_t *p, uint8_t type, uint16_t flags) {
    p[4] = type;
    p[5] = TCP_PROTO_VERSION;
    p[6] = (uint8_t)(flags >> 8);
    p[7] = (uint8_t)flags;
}

/* v1.2 可靠下行：MSG_DATA 的 flags 低 16 位复用为数据序列号 seq（host→guest） */
static inline void tcp_hdr_set_seq(uint8_t *p, uint16_t seq) {
    p[6] = (uint8_t)(seq >> 8);
    p[7] = (uint8_t)seq;
}
static inline uint16_t tcp_hdr_get_seq(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[6] << 8) | p[7]);
}

/* 解析会话协议头（纯逻辑，可宿主 fuzz）。
 * 返回 0=结构合法（session_id/msg_type 出参）；-1=非法（长度不足/版本不符）。
 * 仅校验"结构合法性"，msg_type 的取值与方向合法性由调用方按 §2.3 判定。
 * v1.2：不再要求 flags==0——MSG_DATA 用 flags 低 16 位携带数据序列号 seq。 */
static inline int tcp_parse_hdr(const uint8_t *p, uint32_t n,
                                uint32_t *session_id, uint8_t *msg_type) {
    if (!p || n < TCP_PHDR) return -1;
    if (p[5] != TCP_PROTO_VERSION) return -1;
    if (session_id) *session_id = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                                  ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    if (msg_type) *msg_type = p[4];
    return 0;
}

#endif /* NET_TCP_PROTO_H */