/* mini-os/v2-c-kernel/src/net/netif.c
 * netif 注册表 + 分发（路线图 v1.1 Step 1，D2 注册表模式）。
 * 纯逻辑：只维护"已注册后端阵列 + 当前网卡"，无内核/硬件依赖 => 可宿主单测。
 * 协议层（netsock）只需要 netif_xxx 这四个转发入口，不感知具体网卡。
 */
#include "netif.h"

static const netif_ops_t *netifs[NETIF_MAX];
static int netif_count = 0;
static int netif_current = -1;   /* 当前网卡索引；-1=尚未选定/无可用 */

int netif_register(const netif_ops_t *ops) {
    if (!ops || netif_count >= NETIF_MAX) return -1;
    netifs[netif_count++] = ops;
    return 0;
}

int netif_init_all(void) {
    /* 逐个探测（每个后端 init 一次），把第一个 init 成功的选为当前网卡（D6：按序取第一个） */
    for (int i = 0; i < netif_count; i++) {
        if (netifs[i]->init() == 0 && netif_current < 0)
            netif_current = i;
    }
    return netif_current >= 0 ? 0 : -1;
}

int netif_select(int idx) {
    if (idx < 0 || idx >= netif_count) return -1;
    netif_current = idx;
    return 0;
}

static const netif_ops_t *cur(void) {
    if (netif_current < 0 || netif_current >= netif_count) return 0;
    return netifs[netif_current];
}

int netif_ready(void) {
    const netif_ops_t *c = cur();
    return c ? c->ready() : -1;
}

int netif_tx(const uint8_t *ip, uint32_t len) {
    const netif_ops_t *c = cur();
    return c ? c->tx(ip, len) : -1;
}

int netif_rx(uint8_t *buf, uint32_t max, uint32_t *len) {
    const netif_ops_t *c = cur();
    return c ? c->rx(buf, max, len) : -1;
}

const uint8_t *netif_mac(void) {
    const netif_ops_t *c = cur();
    return c ? c->mac() : 0;
}