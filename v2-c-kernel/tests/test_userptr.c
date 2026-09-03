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
 * 这里提供"假页表"可判定实现：用户半区仅低区 [0x80000000,0x80100000) 与末页
 * 0x80FFF000"已映射"，其余为空洞页 —— 用于宿主单测验证 user_ptr_valid 的
 * "区间 + 逐页映射"双重预检（空洞页拒绝在触碰内存前返回 0，不会段错误）。
 * 真实页表预检由 abuse/其它 QEMU 回归覆盖。 */
static int fake_mapped(uint32_t virt) {
    uint32_t a = virt & ~0xFFFu;
    if (a >= 0x80000000u && a < 0x80100000u) return 1;   /* 低区 app/栈/brk */
    if (a == 0x80FFF000u) return 1;                       /* 用户空间末页 */
    return 0;                                             /* 其余空洞 */
}
int is_mapped(uint32_t virt) { return fake_mapped(virt); }

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

    /* user_ptr_valid：区间内逐页映射预检（假页表：空洞页拒绝） */
    CHECK(user_ptr_valid((void *)0x80500000u, 8) == 0);            /* 区间内空洞页 */
    CHECK(user_ptr_valid((void *)0x80100000u, 4) == 0);            /* 已映射区上界(空洞) */
    CHECK(user_ptr_valid((void *)0x800FFFF0u, 32) == 0);           /* 跨页：进 0x80100000 空洞 */
    CHECK(user_ptr_valid((void *)0x80000000u, 0x2000) == 1);       /* 连续两已映射页 */
    CHECK(user_ptr_valid((void *)0x80FFF000u, 4) == 1);            /* 用户空间末页 */
    /* len=0 跳过映射循环（不要求目标已映射） */
    CHECK(user_ptr_valid((void *)0x80500000u, 0) == 1);

    /* copyin / copyout：拒绝路径（在触碰内存前返回 -1） */
    char kern[16];
    CHECK(copyin((void *)0x7FFFFFFFu, kern, 4) == -1);
    CHECK(copyin((void *)0x00000000u, kern, 4) == -1);
    CHECK(copyin((void *)0xFFFFFFFFu, kern, 4) == -1);
    CHECK(copyout(kern, (void *)0x7FFFFFFFu, 4) == -1);
    CHECK(copyout(kern, (void *)0xFFFFFFFFu, 4) == -1);
    CHECK(copyin((void *)0x80500000u, kern, 4) == -1);             /* 空洞页拒绝 */
    CHECK(copyout(kern, (void *)0x80500000u, 4) == -1);            /* 空洞页拒绝 */

    /* copyin_str：拒绝路径 */
    char dst[16];
    CHECK(copyin_str((void *)0x7FFFFFFFu, dst, sizeof(dst)) == -1);  /* 非法基址 */
    CHECK(copyin_str((void *)0x00000000u, dst, sizeof(dst)) == -1);
    CHECK(copyin_str((void *)0x80000000u, dst, 0) == -1);            /* max=0 */
    CHECK(copyin_str((void *)0x81000000u, dst, sizeof(dst)) == -1);  /* 越过 END，无 NUL */

    UTEST_SUMMARY("test_userptr");
}
