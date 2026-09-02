/* mini-os/v2-c-kernel/tests/test_heredoc.c
 * 宿主侧单测：shell writefile heredoc 的 DELIM 终结判定（shell_heredoc.h）。
 * 回归 PR #25 审核 bug：曾以"path 长度"当 DELIM 长度比较 → EOF 永不匹配、heredoc 永不终结、
 * 后续命令全被吞进收集循环（坑 5 稳定失败，本地未验证直达 CI）。本测试用宿主秒级验证该判定：
 *   1) DELIM 精确匹配=1；行长≠DELIM 长=0（path length 曾误用进此的根因回归）
 *   2) 去头尾空白后匹配仍=1；仅前缀相同+多出字符=0（防前缀截断误判终止）
 *   3) 空行 / 全空白行 = 0（不终结，保行结构）
 * 用法：并入 run_host_tests.sh；exit 0=全绿 1=断言失败。
 */
#include "shell_heredoc.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("[FAIL] %s\n", msg); fails++; } \
    else { printf("[ok]   %s\n", msg); } \
} while (0)

int main(void) {
    /* 1) 精确匹配：EOF(3) 顶到 DELIM 长度 3 */
    CHECK(wf_delim_hit((const uint8_t *)"EOF", 3, (const uint8_t *)"EOF", 3) == 1,
          "DELIM 精确匹配(=1)");
    /* 根因回归：path 长度(如 /multi.c=8)误当 DELIM 长度 → EOF(3) 行长不算 8 → 本应=1 */
    CHECK(wf_delim_hit((const uint8_t *)"EOF", 3, (const uint8_t *)"EOF", 8) == 0,
          "根因回归：误用 path 长度 8 判 EOF(3) 行长=-> 0(旧代码必一直错)");
    CHECK(wf_delim_hit((const uint8_t *)"EOF", 3, (const uint8_t *)"EOF", 3) == 1,
          "DELIM 长度=3 时 EOF 判定=1(修复后成立)");
    /* 2) 去头尾空白后匹配 */
    CHECK(wf_delim_hit((const uint8_t *)"  EOF  ", 7, (const uint8_t *)"EOF", 3) == 1,
          "带头尾空格仍匹配(=1)");
    CHECK(wf_delim_hit((const uint8_t *)"\tEOF\t", 5, (const uint8_t *)"EOF", 3) == 1,
          "带 tab 仍匹配(=1)");
    /* 3) 前缀/超长不误判终止 */
    CHECK(wf_delim_hit((const uint8_t *)"EOFX", 4, (const uint8_t *)"EOF", 3) == 0,
          "DELIM 前缀后多字符(=0, 不终结)");
    CHECK(wf_delim_hit((const uint8_t *)"EO", 2, (const uint8_t *)"EOF", 3) == 0,
          "DELIM 子串(=0, 不终结)");
    /* 4) 空行 / 全空白行 */
    CHECK(wf_delim_hit((const uint8_t *)"", 0, (const uint8_t *)"EOF", 3) == 0,
          "空行(=0, 不终结、保行结构)");
    CHECK(wf_delim_hit((const uint8_t *)"   ", 3, (const uint8_t *)"EOF", 3) == 0,
          "全空白行(=0, 不误判)");
    /* 5) 内容行（源码）不命中 */
    CHECK(wf_delim_hit((const uint8_t *)"int main(){", 11, (const uint8_t *)"EOF", 3) == 0,
          "普通源码行(=0, 写入)");

    printf("test_heredoc: pass=%d fail=%d\n", 9 - fails, fails);
    return fails ? 1 : 0;
}