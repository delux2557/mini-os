/* mini-os/v2-c-kernel/src/userptr.c
 * 用户指针安全访问实现（v0.17）：syscall 边界校验。
 * 校验通过后内核直接读/写用户内存（当前 CR3 即用户页目录，用户半区已映射）。
 */
#include "userptr.h"

int user_ptr_valid(const void *p, uint32_t len) {
    uint32_t a = (uint32_t)p;
    if (a < USER_SPACE_BASE) return 0;                 /* 指向内核低地址，拒绝 */
    if (a > USER_SPACE_END) return 0;                  /* 超过上限（防下方减法回绕） */
    if (len > USER_SPACE_END - a) return 0;            /* 区间越界 / 长度回绕 */
    return 1;
}

int copyin(const void *user_src, void *kern_dst, uint32_t len) {
    if (!user_ptr_valid(user_src, len)) return -1;
    const uint8_t *s = (const uint8_t *)user_src;
    uint8_t *d = (uint8_t *)kern_dst;
    while (len--) *d++ = *s++;
    return 0;
}

int copyout(const void *kern_src, void *user_dst, uint32_t len) {
    if (!user_ptr_valid(user_dst, len)) return -1;
    const uint8_t *s = (const uint8_t *)kern_src;
    uint8_t *d = (uint8_t *)user_dst;
    while (len--) *d++ = *s++;
    return 0;
}

int copyin_str(const void *user_src, char *kern_dst, uint32_t max) {
    if (max == 0) return -1;
    if ((uint32_t)user_src < USER_SPACE_BASE) return -1;
    uint32_t i = 0;
    while (i + 1 < max) {
        uint32_t a = (uint32_t)user_src + i;
        if (a >= USER_SPACE_END) return -1;            /* 越过用户空间上限，无 NUL */
        char c = ((const char *)user_src)[i];
        kern_dst[i] = c;
        if (c == 0) return (int)i;
        i++;
    }
    kern_dst[max - 1] = 0;                             /* 超长：截断 */
    return (int)(max - 1);
}
