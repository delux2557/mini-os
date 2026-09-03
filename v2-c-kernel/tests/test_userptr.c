/* mini-os/v2-c-kernel/tests/test_userptr.c
 * v0.17 用户指针校验宿主单元测试：验证 user_ptr_valid / copyin / copyout / copyin_str 边界。
 * 布局：USER_SPACE_BASE=0x80000000, USER_SPACE_END=0x81000000（v0.26#3 用户空间扩到 16MB）
 * 说明：成功路径会真实触碰内存（QEMU 回归覆盖，见 abuse 应用）；这里只测
 * 纯逻辑边界与"拒绝路径"（在触碰内存前即返回 -1，不会段错误）。
 * 编译：gcc -Isrc src/userptr.c tests/test_userptr.c -o tests/build/test_userptr */
#include "userptr.h"
#include "utest.h"
#include <stdint.h>

/* @修复 stub：sycall 预检后 is_mapped 引入 mem.o 内核依赖，宿主单测无法链接；
 * 这里提供"全映射"可判定假实现，保持拒绝路径（在触碰内存前返 -1）断言不变。
 * 真实页表预检由 abuse/其它 QEMU 回归覆盖。 */
int is_mapped(uint32_t virt) { (void)virt; return 1; }

int main(void) {
    /* user_ptr_valid：纯逻辑边界 */
    CHECK(user_ptr_valid((void *)0x80000000u, 0) == 1);            /* 用户空间起点 */
    CHECK(user_ptr_valid((void *)0x80000000u, 4) == 1);
    CHECK(user_ptr_valid((void *)0x80FFFFFCu, 4) == 1);            /* 上限内末 4 字节 */
    CHECK(user_ptr_valid((void *)0x81000000u, 0) == 1);            /* END 本身（len=0） */
    CHECK(user_ptr_valid((void *)0x81000000u, 1) == 0);            /* 越过 END */
    CHECK(user_ptr_valid((void *)0x80000000u, 0x1000001u) == 0);   /* 长度溢出上界（16MB+1） */
    CHECK(user_ptr_valid((void *)0x7FFFFFFFu, 4) == 0);            /* 内核低地址 */
    CHECK(user_ptr_valid((void *)0x00000000u, 4) == 0);            /* 地址 0 */
    CHECK(user_ptr_valid((void *)0x00001000u, 4) == 0);            /* 低地址内核 */
    CHECK(user_ptr_valid((void *)0xFFFFFFFFu, 1) == 0);            /* 高地址回绕 */
    CHECK(user_ptr_valid((void *)0xFFFFFFFFu, 4) == 0);

    /* copyin / copyout：拒绝路径（在触碰内存前返回 -1） */
    char kern[16];
    CHECK(copyin((void *)0x7FFFFFFFu, kern, 4) == -1);
    CHECK(copyin((void *)0x00000000u, kern, 4) == -1);
    CHECK(copyin((void *)0xFFFFFFFFu, kern, 4) == -1);
    CHECK(copyout(kern, (void *)0x7FFFFFFFu, 4) == -1);
    CHECK(copyout(kern, (void *)0xFFFFFFFFu, 4) == -1);

    /* copyin_str：拒绝路径 */
    char dst[16];
    CHECK(copyin_str((void *)0x7FFFFFFFu, dst, sizeof(dst)) == -1);  /* 非法基址 */
    CHECK(copyin_str((void *)0x00000000u, dst, sizeof(dst)) == -1);
    CHECK(copyin_str((void *)0x80000000u, dst, 0) == -1);            /* max=0 */
    CHECK(copyin_str((void *)0x81000000u, dst, sizeof(dst)) == -1);  /* 越过 END，无 NUL */

    UTEST_SUMMARY("test_userptr");
}
