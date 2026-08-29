/* mini-os/v2-c-kernel/src/userptr.h
 * 用户指针安全访问（v0.17）：syscall 边界校验（copyin/copyout）。
 * 内核按低地址恒等映射（内核内存全部 < USER_SPACE_BASE），用户内存位于高地址半区；
 * copyin/copyout 先校验 [p, p+len) 完整落在用户空间内，再直接拷贝
 * （内核当前地址空间即用户页目录，可直接寻址用户内存）。
 * 纯逻辑：只依赖本文件常量，可宿主单测（tests/test_userptr.c）。
 */
#ifndef _USERPTR_H
#define _USERPTR_H
#include <stdint.h>

/* 用户空间 [USER_SPACE_BASE, USER_SPACE_END)：代码页 0x80000000 / 栈区 /
 * 共享区 0x80044000 / app 槽 0x80040000 均在内。END 取 0x80100000 作防回绕上界
 * （比实际映射区更宽裕；校验目标是"不得指向内核低地址"，上界只是安全侧收紧）。 */
#define USER_SPACE_BASE 0x80000000u
#define USER_SPACE_END  0x80100000u

/* [p, p+len) 是否全部落在用户空间（含上界与回绕保护）；len 可为 0（空指针需单独判定） */
int user_ptr_valid(const void *p, uint32_t len);

/* 拷贝 len 字节 用户<->内核；校验失败返回 -1 */
int copyin (const void *user_src, void *kern_dst, uint32_t len);
int copyout(const void *kern_src, void *user_dst, uint32_t len);

/* 把 NUL 结尾字符串拷入内核缓冲（最多 max-1 字符 + NUL）：
 * 返回长度（不含 NUL）；非法基址 / 越过用户空间上限返回 -1；超长则截断并返回 max-1 */
int copyin_str(const void *user_src, char *kern_dst, uint32_t max);

#endif
