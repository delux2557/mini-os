/* mini-os/v2-c-kernel/src/syscall_table.h
 * 系统调用号表——唯一事实源（R1.1，外部审计 A5 / TD-06）。
 * 之前号表分散三处（user_lib.h 宏 / userprog.c 宏 / usermode.c 裸数字 case）手工同步；
 * 现以 X-Macro 单源定义，user_lib.h / userprog.c（enum 展开）与 usermode.c（case 标签 +
 * 名表 + ABI 版本）全部从本表派生——加一个 syscall 只改本表 + usermode.c 分发表实现。
 *
 * 用法：`#define SYS_TABLE(X) ...` 后以三种展开器消费：
 *   - enum 常量：`#define ENTRY(num,name,mask) name = num,` + `enum { SYS_TABLE(ENTRY) };`
 *   - 名表（内核 audit）：`#define ENTRY(num,name,mask) { num, #name },`
 *   - 默认掩码：mask 位=0 默认允许（per-process sc_mask 由 sys_limit 运行时只收窄）。
 *
 * 注意：
 *   - 39（SYS_DHCP_TICK）已撤销（v0.38 R1.3 后半，#147），号保留**不复用**（历史 ABI 空洞）。
 *   - 号一旦发布不得改号（ABI 稳定）；新增号只能追加。
 */
#ifndef MINIOS_SYSCALL_TABLE_H
#define MINIOS_SYSCALL_TABLE_H

/* ABI 版本（v1.5 R1.1 引入）：syscall 表/语义演进的版本戳，
 * 经 SYS_ABI_VERSION 查询，sys_kern_audit 报告。 */
#define SYSCALL_ABI_VERSION 1

#define SYS_TABLE(X) \
    X(0,  SYS_EXIT,          0) \
    X(1,  SYS_PRINT,         0) \
    X(2,  SYS_GET_TICKS,     0) \
    X(3,  SYS_SLEEP,         0) \
    X(4,  SYS_YIELD,         0) \
    X(5,  SYS_GET_PID,       0) \
    X(6,  SYS_SEM_CREATE,    0) \
    X(7,  SYS_SEM_WAIT,      0) \
    X(8,  SYS_SEM_SIGNAL,    0) \
    X(9,  SYS_SHMEM,         0) \
    X(10, SYS_MSG_CREATE,    0) \
    X(11, SYS_MSG_SEND,      0) \
    X(12, SYS_MSG_RECV,      0) \
    X(13, SYS_FS_CREATE,     0) \
    X(14, SYS_FS_OPEN,       0) \
    X(15, SYS_FS_WRITE,      0) \
    X(16, SYS_FS_READ,       0) \
    X(17, SYS_FS_CLOSE,      0) \
    X(18, SYS_FS_LS,         0) \
    X(19, SYS_FS_DELETE,     0) \
    X(20, SYS_READLINE,      0) \
    X(21, SYS_SPAWN_FILE,    0) \
    X(22, SYS_WAIT,          0) \
    X(23, SYS_MAP_PAGE,      0) \
    X(24, SYS_FORK,          0) \
    X(25, SYS_EXEC,          0) \
    X(26, SYS_FS_SEEK,       0) \
    X(27, SYS_FS_MKDIR,      0) \
    X(28, SYS_FS_RMDIR,      0) \
    X(29, SYS_FS_SYNC,       0) \
    X(30, SYS_NET_SOCKET,    0) \
    X(31, SYS_NET_SENDTO,    0) \
    X(32, SYS_NET_RECVFROM,  0) \
    X(33, SYS_NET_CLOSE,     0) \
    X(34, SYS_KERN_AUDIT,    0) \
    X(35, SYS_BRK,           0) \
    X(36, SYS_LIMIT,         0) \
    X(37, SYS_FS_READDIR,    0) \
    X(38, SYS_NETDIAG,       0) \
    X(39, SYS_DHCP_TICK,    0)  /* 已撤销（v0.38 R1.3 后半），号保留不复用（历史 ABI 空洞） */ \
    X(40, SYS_DHCP_QUERY,    0) \
    X(41, SYS_DHCP_SEND,     0) \
    X(42, SYS_DHCP_RECV,     0) \
    X(43, SYS_DHCP_APPLY,    0) \
    X(44, SYS_DHCP_FALLBACK, 0) \
    X(45, SYS_ABI_VERSION,   0)

#endif /* MINIOS_SYSCALL_TABLE_H */
