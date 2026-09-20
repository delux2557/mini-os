/* mini-os/v2-c-kernel/tests/test_logthrottle.c
 * 宿主单测：日志限流助手（src/kernel/logthrottle.h）——把"日志有界"这条防线钉在纯逻辑层。
 *
 * 为什么值得单测：guest 侧的断言（qemu_regression 跑 logflood 数串口行数）能证"端到端生效"，
 * 但它慢、且依赖 QEMU/串口；而"首次 + 每 N 次"的**边界语义**（第 1 次必打、第 N 次必打、
 * 中间静默、period=0 不限流）用宿主单测可逐条精确锁死，秒级反馈。二者互补。 */
#include <stdint.h>
#include "logthrottle.h"
#include "utest.h"

int main(void) {
    /* 1) 默认周期 64：只有 n=1 与 n%64==0 打 */
    log_throttle_t t = {0};
    int hits = 0, first_hit = -1, last_hit = -1;
    for (uint32_t n = 1; n <= 200; n++) {
        if (log_throttle_hit(&t, LOG_THROTTLE_PERIOD)) {
            hits++;
            if (first_hit < 0) first_hit = (int)n;
            last_hit = (int)n;
        }
    }
    CHECK_EQ(t.n, 200);          /* 调用次数被完整计数（抑制量可见） */
    CHECK_EQ(hits, 4);           /* 200 次 ⇒ 4 条：1/64/128/192 */
    CHECK_EQ(first_hit, 1);      /* 首次必打（否则"曾经发生过"不可见） */
    CHECK_EQ(last_hit, 192);     /* 不超过 200 的最后一个整倍数 */

    /* 2) 边界：恰好在第 N 次打、第 N±1 次不打 */
    log_throttle_t b = {0};
    int b_hits = 0;
    for (uint32_t n = 1; n <= 64; n++) if (log_throttle_hit(&b, 64)) b_hits++;
    CHECK_EQ(b_hits, 2);         /* 1 与 64 */
    CHECK(log_throttle_hit(&b, 64) == 0);   /* 第 65 次：静默 */
    CHECK(log_throttle_hit(&b, 64) == 0);   /* 第 66 次：静默 */

    /* 3) period=0 ⇒ 显式不限流（且不得除零崩） */
    log_throttle_t z = {0};
    int z_hits = 0;
    for (uint32_t n = 1; n <= 10; n++) if (log_throttle_hit(&z, 0)) z_hits++;
    CHECK_EQ(z_hits, 10);

    /* 4) period=1 ⇒ 每次都打（退化但不崩） */
    log_throttle_t o = {0};
    int o_hits = 0;
    for (uint32_t n = 1; n <= 5; n++) if (log_throttle_hit(&o, 1)) o_hits++;
    CHECK_EQ(o_hits, 5);

    /* 5) 洪泛有界性：N 次调用 ⇒ 恰好 1 + floor(N/period) 条（这是"防线"的定量表述） */
    log_throttle_t big = {0};
    int big_hits = 0;
    const uint32_t N = 100000u;                    /* 非 64 倍数，覆盖"末段不足一个周期" */
    for (uint32_t n = 1; n <= N; n++) if (log_throttle_hit(&big, 64)) big_hits++;
    CHECK_EQ(big_hits, 1 + (int)(N / 64u));        /* n=1 一条 + 每 64 一条 */

    UTEST_SUMMARY("test_logthrottle");
}
