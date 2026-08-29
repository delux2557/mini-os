/* mini-os/v2-c-kernel/tests/utest.h
 * 极简宿主测试断言框架：只依赖标准 C，无第三方库。
 * CHECK(条件) / CHECK_EQ(实际, 期望)，main 末尾调用 UTEST_SUMMARY("套件名")。 */
#ifndef _UTEST_H
#define _UTEST_H

#include <stdio.h>
#include <stdint.h>

static int utest_pass = 0, utest_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { utest_pass++; } \
        else { utest_fail++; \
               fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ(actual, expected) \
    do { \
        uintptr_t _a = (uintptr_t)(actual); \
        uintptr_t _e = (uintptr_t)(expected); \
        if (_a == _e) { utest_pass++; } \
        else { utest_fail++; \
               fprintf(stderr, "  FAIL %s:%d: %s == %s (got %lu, want %lu)\n", \
                       __FILE__, __LINE__, #actual, #expected, \
                       (unsigned long)_a, (unsigned long)_e); } \
    } while (0)

#define UTEST_SUMMARY(name) \
    do { \
        printf("[%s] pass=%d fail=%d\n", name, utest_pass, utest_fail); \
        return utest_fail == 0 ? 0 : 1; \
    } while (0)

#endif
