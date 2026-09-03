/* mini-os/v2-c-kernel/src/userptr.c
 * 用户指针安全访问实现（v0.17）：syscall 边界校验。
 * 校验通过后内核直接读/写用户内存（当前 CR3 即用户页目录，用户半区已映射）。
 * v0.17 修复：预检从"纯区间"升级为"区间 + 逐页已映射"，见 user_ptr_valid 注释。
 */
#include "userptr.h"
#include "mem.h"   /* is_mapped() 逐页预检，封堵"区间内空洞页"整机宕机 */

/* 校验 [p, p+len) 全部落在用户空间(无回绕)，且覆盖的每一页均已映射。
 * 用户半区 [BASE,END) 绝大多数页是空洞：逐页预检让 syscall 在触碰前返回 0，
 * 避免内核态读未映射页 -> 懒分配区外 -> pf_handler 落到 [FATAL] cli;hlt 整机停机
 * （单用户进程即可 DoS 整个内核）。
 * 前提：本内核 syscall 期间不替换页表，mem_current_pd() 即目标进程页目录，
 * 检查后不会变化（无 TOCTOU）；len 可为 0（空指针需单独判定）。 */
int user_ptr_valid(const void *p, uint32_t len) {
    uint32_t a = (uint32_t)p;
    if (a < USER_SPACE_BASE) return 0;                 /* 指向内核低地址，拒绝 */
    if (a > USER_SPACE_END) return 0;                  /* 超过上限（防下方减法回绕） */
    if (len > USER_SPACE_END - a) return 0;            /* 区间越界 / 长度回绕 */
    if (len) {
        uint32_t end = a + len;
        for (uint32_t pg = a & ~0xFFFu; pg < end; pg = (pg & ~0xFFFu) + 0x1000u)
            if (!is_mapped(pg)) return 0;              /* 命中空洞页：拒绝 */
    }
    return 1;
}

int copyin(const void *user_src, void *kern_dst, uint32_t len) {
    if (!user_ptr_valid(user_src, len)) return -1;     /* 区间 + 逐页映射预检 */
    const uint8_t *s = (const uint8_t *)user_src;
    uint8_t *d = (uint8_t *)kern_dst;
    while (len--) *d++ = *s++;
    return 0;
}

int copyout(const void *kern_src, void *user_dst, uint32_t len) {
    if (!user_ptr_valid(user_dst, len)) return -1;     /* 区间 + 逐页映射预检 */
    const uint8_t *s = (const uint8_t *)kern_src;
    uint8_t *d = (uint8_t *)user_dst;
    while (len--) *d++ = *s++;
    return 0;
}

int copyin_str(const void *user_src, char *kern_dst, uint32_t max) {
    if (max == 0) return -1;
    if ((uint32_t)user_src < USER_SPACE_BASE) return -1;
    uint32_t i = 0;
    uint32_t last_pg = (uint32_t)-1;   /* copyin_str 未知长度(到 NUL)，仅跨页处查映射 */
    while (i + 1 < max) {
        uint32_t a = (uint32_t)user_src + i;
        if (a >= USER_SPACE_END) return -1;
        uint32_t pg = a & ~0xFFFu;
        if (pg != last_pg) {
            last_pg = pg;
            if (!is_mapped(pg)) return -1;   /* 命中空洞页：拒绝，避免内核态缺页整机停机 */
        }
        char c = ((const char *)user_src)[i];
        kern_dst[i] = c;
        if (c == 0) return (int)i;
        i++;
    }
    kern_dst[max - 1] = 0;                             /* 超长：截断 */
    return (int)(max - 1);
}