/* mini-os/v2-c-kernel/tests/test_netif.c
 * 宿主单元测试：netif 注册表 + 分发（v1.1 Step 1）。
 * netif.c 是纯逻辑（无内核/硬件依赖），用一个 mock ops 表驱动网卡验证：
 *  - 注册首个后端即被选用，netif_xxx 转发到当前网卡
 *  - 无网卡（init 失败）时 netif_ready()/tx()/rx() 优雅返回 -1
 *  - D6：netif_select 显式切换后端
 */
#include "netif.h"
#include "utest.h"
#include <string.h>

static int a_init_calls = 0, a_tx_calls = 0;
static int b_init_calls = 0, b_tx_calls = 0;
static uint8_t a_macbuf[6] = {0x52, 0x54, 0x00, 0x11, 0x22, 0x33};
static uint8_t rxbuf[16];
static uint32_t rxlen_g = 0;

static int a_init(void) { a_init_calls++; return 0; }
static int a_ready(void) { return 0; }
static int a_tx(const uint8_t *ip, uint32_t len) { (void)ip; (void)len; a_tx_calls++; return 0; }
static int a_rx(uint8_t *buf, uint32_t max, uint32_t *len) {
    (void)max; memcpy(buf, rxbuf, rxlen_g); *len = rxlen_g; return 1;
}
static const uint8_t *a_mac(void) { return a_macbuf; }

static int b_init(void) { b_init_calls++; return 0; }
static int b_ready(void) { return -1; }
static int b_tx(const uint8_t *ip, uint32_t len) { (void)ip; (void)len; b_tx_calls++; return -1; }
static int b_rx(uint8_t *buf, uint32_t max, uint32_t *len) { (void)buf; (void)max; *len = 0; return 0; }
static const uint8_t *b_mac(void) { return 0; }

static const netif_ops_t ops_a = { a_init, a_ready, a_tx, a_rx, a_mac };
static const netif_ops_t ops_b = { b_init, b_ready, b_tx, b_rx, b_mac };

int main(void) {
    /* 空注册表：无网卡，接口优雅返回 -1 */
    CHECK_EQ(netif_ready(), -1);
    CHECK_EQ(netif_tx(0, 0), -1);
    CHECK_EQ(netif_rx(0, 0, 0), -1);
    CHECK(netif_mac() == 0);

    /* 注册：首个成功注册且 init()==0 的成为当前网卡 */
    CHECK_EQ(netif_register(&ops_a), 0);
    CHECK_EQ(netif_register(&ops_b), 0);
    CHECK_EQ(netif_register(0), -1);             /* 拒绝 NULL */

    /* netif_init_all：遍历拉活，选出 init 成功的后端（此处 ops_a 在前） */
    CHECK_EQ(netif_init_all(), 0);
    CHECK_EQ(a_init_calls, 1);
    CHECK_EQ(b_init_calls, 1);
    CHECK_EQ(netif_ready(), 0);
    CHECK(netif_mac() == (const uint8_t *)a_macbuf);
    CHECK_EQ(netif_tx(0, 0), 0);                 /* 转发到当前（ops_a） */
    CHECK_EQ(a_tx_calls, 1);

    /* tx/rx 分发改到当前网卡 */
    rxbuf[0] = 0xAB; rxlen_g = 1;
    uint8_t out[16] = {0}; uint32_t len = 0;
    CHECK_EQ(netif_rx(out, sizeof out, &len), 1);
    CHECK_EQ(out[0], 0xABu);
    CHECK_EQ(len, 1u);

    /* D6 预留：用 netif_select 显式切到第二个后端（即使其 ready=-1，也只做路由） */
    CHECK_EQ(netif_select(1), 0);
    CHECK_EQ(netif_tx(0, 0), -1);                /* 转发到 ops_b */
    CHECK_EQ(b_tx_calls, 1);
    CHECK(netif_mac() == 0);                     /* ops_b 无 MAC */
    CHECK_EQ(netif_select(99), -1);              /* 越界拒绝 */

    UTEST_SUMMARY("test_netif");
}