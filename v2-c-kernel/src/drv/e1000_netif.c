/* mini-os/v2-c-kernel/src/drv/e1000_netif.c
 * e1000 网卡 -> netif 适配层（路线图 v1.1 Step 1）。
 * 把现有 e1000 驱动函数包进 netif_ops 表；**驱动本身（e1000.c）不改逻辑**，只做薄封装。
 * 贡献给 netif 层的职责 = **以太网链路层封装/解封装**（D1）：
 *   - tx：给上层 IP 数据报加以太网头（dst=网关 MAC，src=本机 MAC，ethertype=IPv4）
 *   - rx：剥掉以太网头、只把 **IP 数据报**交给上层；非 IPv4 帧（如残留 ARP）消费并丢弃
 * 驱动是内核态（MMIO 高地址访问须临时切内核页目录），故本适配器是内核依赖，非纯逻辑。
 */
#include "netif.h"
#include "e1000.h"
#include "mem.h"
#include <stdint.h>

#define NET_ETH_TYPE_IPV4 0x0800u

/* netsock 同款：e1000 MMIO 位于高地址（PDE>=512），用户页目录只克隆低 1GB PDE，
 * 凡收发（MMIO 访问）须临时切到内核页目录。该职责随适配层下沉到此。 */
static void enter_kernel_pd(uint32_t *saved) {
    *saved = mem_current_pd();
    switch_page_dir(mem_kernel_pd());
}
static void exit_kernel_pd(uint32_t saved) { switch_page_dir(saved); }

static int e1000_if_init(void) { return e1000_init(); }
static int e1000_if_ready(void) { return e1000_ready(); }

/* tx：给 IP 数据报加以太网头（经网关 MAC 寻址）后交给驱动发送 */
static int e1000_if_tx(const uint8_t *ip, uint32_t len) {
    const uint8_t *gw = e1000_gw_mac();
    if (!gw) return -1;
    if (14u + len > 1600u) return -1;
    uint8_t eth[1600];
    for (int i = 0; i < 6; i++) { eth[i] = gw[i]; eth[6 + i] = e1000_mac()[i]; }
    eth[12] = (uint8_t)(NET_ETH_TYPE_IPV4 >> 8);
    eth[13] = (uint8_t)NET_ETH_TYPE_IPV4;
    for (uint32_t i = 0; i < len; i++) eth[14 + i] = ip[i];

    uint32_t saved;
    enter_kernel_pd(&saved);               /* MMIO 访问须在内核页目录下 */
    int rc = e1000_tx(eth, 14 + len);
    exit_kernel_pd(saved);
    return rc;
}

/* rx：取一帧，剥以太网头把 IP 数据报交给上层。
 * 返回 1=收到 IP 数据报 / 0=网卡排空 / -1=消费一帧但非 IPv4（调用方继续排空）。 */
static int e1000_if_rx(uint8_t *buf, uint32_t max, uint32_t *len) {
    uint8_t eth[1600];
    uint32_t elen = 0;
    uint32_t saved;
    enter_kernel_pd(&saved);               /* 排空网卡（e1000_rx 访问 MMIO） */
    int rc = e1000_rx(eth, sizeof eth, &elen);
    exit_kernel_pd(saved);
    if (rc != 1) return rc;                /* 0=无帧  负=失败 */
    if (elen < 14 || eth[12] != (uint8_t)(NET_ETH_TYPE_IPV4 >> 8) ||
        eth[13] != (uint8_t)NET_ETH_TYPE_IPV4)
        return -1;                         /* 非 IPv4（如 ARP），消费并丢弃 */
    uint32_t n = elen - 14;
    if (n > max) return -1;
    for (uint32_t i = 0; i < n; i++) buf[i] = eth[14 + i];
    *len = n;
    return 1;
}

static const uint8_t *e1000_if_mac(void) { return e1000_mac(); }

static const netif_ops_t e1000_netif_ops = {
    e1000_if_init, e1000_if_ready, e1000_if_tx, e1000_if_rx, e1000_if_mac
};

/* 注册 e1000 为当前 netif 后端。由 kernel_main 在 e1000_dhcp 等自检前调用。 */
void e1000_netif_register(void) { netif_register(&e1000_netif_ops); }