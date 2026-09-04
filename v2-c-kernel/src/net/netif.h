/* mini-os/v2-c-kernel/src/net/netif.h
 * netif 抽象层（路线图 v1.1 Step 1，D1/D2）。
 *   - D1：包单位为 **IP 数据报**（lwIP netif 模式）——协议层与"帧"无关；
 *     以太网头 / SLIP 等**链路层封装下沉到各网卡适配层**（e1000 适配器负责 eth 头，
 *     串口适配器负责 SLIP 成帧）。
 *   - D2：接口形态为 **ops 表**（init/ready/tx/rx/mac），注册表模式。协议层只依赖
 *     此接口，不具体的网卡驱动 => 换网卡协议层零改动。本表 = 未来 HAL 设备表原型。
 *   - D6：网卡选择策略预留——当前单网卡按注册顺序取第一个就绪的；未来可按目的 IP
 *     选路平滑升级（netif_select 留作策略接口）。
 */
#ifndef NET_NETIF_H
#define NET_NETIF_H

#include <stdint.h>

#define NETIF_MAX 4   /* 注册表容量（当前 2 及时够用：e1000 + 串口） */

/* 单个以太网帧上限（MTU1500 载荷 + 14B 链路头 = 1514；留 4B 裕量到 1518）。
 * SEC-07：DHCP 续约链在 IRQ0 中断栈（4KB）上层层拷贝整帧，据此把各接收缓冲
 * 钳到恰好容纳一帧的最小值，避免 4 倍放大叠加超过内核栈。 */
#define NET_ETH_FRAME_MAX 1518

typedef struct netif_ops {
    int (*init)(void);                 /* 探测+初始化驱动；成功 0，无硬件 -1 */
    int (*ready)(void);                /* 驱动是否就绪：0 就绪 */
    /* 发送一个 IP 数据报（链路层封装由适配器完成）；成功 0 */
    int (*tx)(const uint8_t *ip, uint32_t len);
    /* 收一个 IP 数据报（链路层头由适配器剥除后留 IP 数据报）：
     *   1 = 收到一个 IP 数据报（len 置为数据报长度，内容在 buf）
     *   0 = 网卡排空（无更多帧）
     *  -1 = 消费了一帧但非 IP 数据报（调用方应继续排空下一帧） */
    int (*rx)(uint8_t *buf, uint32_t max, uint32_t *len);
    const uint8_t *(*mac)(void);       /* 链路层地址访问器；无 MAC（如 SLIP）返回 NULL */
} netif_ops_t;

/* 注册一个 netif 后端（首个注册且 init 成功的成为当前网卡）。成功 0 */
int netif_register(const netif_ops_t *ops);
/* 初始化全部已注册后端，并把"第一个 init 成功"的选为当前网卡。成功 0，无可用网卡 -1 */
int netif_init_all(void);
/* D6 预留：显式选择第 idx 个已注册网卡为当前（0 或 -1）。当前单网卡阶段一般不用 */
int netif_select(int idx);

/* ---- 送往"当前网卡"的转发函数（协议层只用这四个）---- */
int  netif_ready(void);
int  netif_tx(const uint8_t *ip, uint32_t len);
int  netif_rx(uint8_t *buf, uint32_t max, uint32_t *len);
const uint8_t *netif_mac(void);

#endif /* NET_NETIF_H */