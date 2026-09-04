/* mini-os/v2-c-kernel/tools/ksym_stub.c
 * 仅用于两阶段链接 phase-1：给 ksym.o 的 ksym_tab/ksym_count 提供空占位定义，
 * 使无符号表内核可链接成功以生成符号表；phase-2 链接真实 ksym_tab.o 时本 stub
 * 不参与，不会产生重复符号。
 */
struct ksym { unsigned int addr; const char *name; };
const struct ksym ksym_tab[] = { { 0u, (const char *)0 } };
const unsigned int ksym_count = 0;