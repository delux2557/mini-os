/* mini-os/v2-c-kernel/src/app/shell_heredoc.h
 * shell writefile heredoc 的"DELIM 终结判定"纯逻辑（v1.4 修复版）。
 * 无 syscall / 用户库依赖，可宿主单测（tests/test_heredoc.c）。单一事实来源：
 *   - 用户侧 shell（src/app/shell.c cmd_writefile）用它判"一行是否等于 DELIM → 终结本写入"
 *   - 宿主单测（tests/test_heredoc.c）用它验证 DELIM 匹配/去空白/空行/长度，防"bug 直达 CI"。
 * 背景 bug（PR #25 审核）: 曾用"path 长度"当 DELIM 长度比较，DELIM 永不匹配、heredoc 永不终结。
 * 故本函数强制以**delim_len 参数**（调用方在 path 解析前保存的 DELIM 长度）作为比对基准。 */
#ifndef APP_SHELL_HEREDOC_H
#define APP_SHELL_HEREDOC_H
#include <stdint.h>

/* 判定一行(line[0..n))，先去头尾空白，看是否恰好等于 delim（长度 dl）。
 * 返回 1 = 命中 DELIM（heredoc 应收尾）；0 = 未命中（含空/全空白行）。
 * 注意：比较长度用 dl（=DELIM 长度），勿在调用方顺带复用的其他变量上栽跟头。 */
static inline int wf_delim_hit(const uint8_t *line, uint32_t n,
                               const uint8_t *delim, uint32_t dl) {
    uint32_t s = 0, e = n, k;
    while (s < e && (line[s] == ' ' || line[s] == '\t')) s++;
    while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t')) e--;
    if (e - s != dl) return 0;              /* 行长 ≠ DELIM 长：不命中（含空行 dl>0） */
    for (k = 0; k < dl; k++)                /* 逐字符相等才命中 */
        if (line[s + k] != delim[k]) return 0;
    return 1;
}

#endif /* APP_SHELL_HEREDOC_H */