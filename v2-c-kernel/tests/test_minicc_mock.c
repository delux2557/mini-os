/* mini-os/v2-c-kernel/tests/test_minicc_mock.c
 * 任务 4/5 的 Mock 白盒单测驱动（原则 6 的 Mock 半兑现）。
 *
 * 思路（对应 docs/design/minicc-v3-后续任务.md 任务4"Mock 注入缝"）：
 *   - `#define MINICC_MOCK` + `#include "../tools/minicc/minicc.c"` 白盒：直接访问 static CC cc
 *     单例与 next_tok()/expr()/fail() 等静态函数。MINICC_MOCK 仅在 `cc` 内追加 fail 的
 *     setjmp/longjmp 注入字段、并在 fail() 开头加长跳转分支；正式构建（host_crt/minicc_crt/
 *     guest/自举）不定义它 → 布局与行为零变化（P1==P2 不破）。
 *   - 不 include host_crt.c（它给 `main`+完整运行时，会撞本驱动的 `main`）；自建 `syscall3`
 *     shim（exit / print / brk 静态竞技场）——lexer/parser 层只依赖 brk(xmalloc)。
 *   - 错误路径注入：先 setjmp(cc.fail_jb) 并置 fail_jmp_on → fail 走 longjmp 回测试现场，
 *     经 cc.fail_msg 就地断言错误消息而不退进程；minicc 停摆、不会再空转出越界态。
 *     这是纯黑盒测不到、Mock 独有增量价值。
 *   - 每次用例 t_reset()：清空 CC 单例全部字段 + 复位 brk，保证用例隔离。
 *
 * 注意（宏碰撞）：minicc.c 顶部的收敛别名 `#define src cc.src` 等会把 `cc.src` 展开成
 *   `cc.cc.src`——驱动对"被别名收编的字段"一律用别名宏读写（src/tok/toklen/tok_is_num/...），
 *   只有未被别名收编的字段（fail_jb/fail_msg/fail_jmp_on）才允许 `cc.xxx` 写法。
 *
 * 编译（宿主）：gcc -m32 -std=gnu99 -O0 -w -DMINICC_MOCK -o <out> tests/test_minicc_mock.c
 *   在 test_minicc.sh 宿主层接入，见 [4/4] Mock 层。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "utest.h"

#define MINICC_MOCK 1
#include "../tools/minicc/minicc.c"

/* ---- 自建 syscall3 shim（exit/print/brk；lexer/parser 只需 brk） ---- */
static unsigned char arena_mem[64u << 20];
static uint32_t base_brk = 0, cur_brk = 0;

int syscall3(int n, int a, int b, int c) {
    (void)b; (void)c;
    switch (n) {
    case 0:  exit((unsigned)a & 255u);                       /* SYS_EXIT */
    case 1:  fwrite((const void *)(uintptr_t)a, 1, strlen((const char *)(uintptr_t)a), stderr);
             return 0;                                       /* SYS_PRINT */
    case 35: /* SYS_BRK：静态竞技场模拟 */
        if (!base_brk) base_brk = (uint32_t)(uintptr_t)arena_mem;
        if (a == 0) return (int)(cur_brk ? cur_brk : base_brk);
        cur_brk = (uint32_t)a;
        return 0;
    default: return 0;
    }
}

/* ---- 用例隔离：清空 CC 单例（含 fail 注入态）+ 复位 brk ---- */
static void t_reset(void) {
    memset(&cc, 0, sizeof cc);
    cur_brk = 0;
}
/* 喂源码（走别名宏，避免 cc.src 触发宏碰撞） */
static void lex_set(const char *s) {
    t_reset();
    src = (const unsigned char *)s;
    src_len = (int)strlen(s);
    src_pos = 0;
}

/* ---- 错误路径捕获：fail 经 longjmp 回到 setjmp 现场；返回捕获消息（无错=本已清 NULL） ---- */
static const char *next_tok_err(void) {
    cc.fail_jmp_on = 1;
    if (setjmp(cc.fail_jb) == 0) next_tok();
    cc.fail_jmp_on = 0;
    return cc.fail_msg;
}
static const char *expr_err(void) {
    cc.fail_jmp_on = 1;
    if (setjmp(cc.fail_jb) == 0) expr();
    cc.fail_jmp_on = 0;
    return cc.fail_msg;
}

/* ---- 断言辅助 ---- */
static void expect_num(Node *n, int v) { CHECK(n != NULL && n->kind == ND_NUM && n->val == v); }
static void expect_sym(Node *n, int k) { CHECK(n != NULL && n->kind == k); }

/* ================= 用例组 1：词法分类（精微 token 断言） ================= */
static void t_lex_num(void) {
    lex_set("123 ");
    next_tok();
    CHECK(tok_is_num && toklen == 3 && strcmp(tok, "123") == 0);
    next_tok();                                  /* EOF */
    CHECK(tok[0] == 0);
}
static void t_lex_word(void) {
    lex_set("abc_1 ");
    next_tok();
    CHECK(tok_is_word && strcmp(tok, "abc_1") == 0);
}
static void t_lex_double_sym(void) {
    lex_set("<= ");
    next_tok();
    CHECK(!tok_is_word && !tok_is_num && !tok_is_str && !tok_is_char
          && toklen == 2 && strcmp(tok, "<=") == 0);
}
static void t_lex_single_sym(void) {
    lex_set("+");
    next_tok();
    CHECK(!tok_is_word && toklen == 1 && strcmp(tok, "+") == 0);
}
static void t_lex_str(void) {
    lex_set("\"hi\"");
    next_tok();
    CHECK(tok_is_str && strcmp(tok, "hi") == 0);
}
static void t_lex_char(void) {
    lex_set("'x'");
    next_tok();
    CHECK(tok_is_char && tok[0] == 'x');
}
static void t_lex_block_comment(void) {
    lex_set(" /* a */ 42");
    next_tok();
    CHECK(tok_is_num && strcmp(tok, "42") == 0);
}
static void t_lex_line_comment(void) {
    lex_set("//x\n 42");
    next_tok();
    CHECK(tok_is_num && strcmp(tok, "42") == 0);
}
static void t_lex_hex_token(void) {
    lex_set("0xff");
    next_tok();
    CHECK(tok_is_num && strcmp(tok, "0xff") == 0);
}

/* ================= 用例组 2：算术优先级建树（AST 形状断言） ================= */
static void t_expr_precedence(void) {
    lex_set("1+2*3");
    next_tok();
    Node *n = expr();                    /* 高优先级 * 绑定更紧 → 根为 ADD */
    expect_sym(n, ND_ADD);
    expect_num(n->l, 1);
    expect_sym(n->r, ND_MUL);
    expect_num(n->r->l, 2);
    expect_num(n->r->r, 3);
}
static void t_expr_precedence_rev(void) {
    lex_set("2*3+1");
    next_tok();
    Node *n = expr();
    expect_sym(n, ND_ADD);
    expect_sym(n->l, ND_MUL);
    expect_num(n->l->l, 2);
    expect_num(n->l->r, 3);
    expect_num(n->r, 1);
}
static void t_expr_left_assoc(void) {
    lex_set("8-5-2");
    next_tok();
    Node *n = expr();                    /* 左结合 → ((8-5)-2) */
    expect_sym(n, ND_SUB);
    expect_sym(n->l, ND_SUB);
    expect_num(n->l->l, 8);
    expect_num(n->l->r, 5);
    expect_num(n->r, 2);
}
static void t_expr_paren(void) {
    lex_set("(1+2)*3");
    next_tok();
    Node *n = expr();                    /* 括号提权 → 根为 MUL */
    expect_sym(n, ND_MUL);
    expect_sym(n->l, ND_ADD);
    expect_num(n->l->l, 1);
    expect_num(n->l->r, 2);
    expect_num(n->r, 3);
}
static void t_expr_hex_value(void) {
    lex_set("0xff");
    next_tok();
    Node *n = expr();
    expect_num(n, 255);
}

/* ================= 用例组 3：错误路径（setjmp 捕获 fail 消息，不退进程） ================= */
static void t_err_unterminated_string(void) {
    lex_set("\"abc");
    const char *m = next_tok_err();
    CHECK(m != NULL && strstr(m, "unterminated string") != NULL);
}
static void t_err_bad_number(void) {
    lex_set("123abc");
    const char *m = next_tok_err();
    CHECK(m != NULL && strstr(m, "bad number") != NULL);
}
static void t_err_bad_escape(void) {
    lex_set("\"a\\q\"");
    const char *m = next_tok_err();
    CHECK(m != NULL && strstr(m, "bad escape") != NULL);
}
static void t_err_bad_expression(void) {
    lex_set("1+");
    next_tok();
    const char *m = expr_err();
    CHECK(m != NULL && strstr(m, "bad expression") != NULL);
}

static void run_all(void) {
    t_lex_num(); t_lex_word(); t_lex_double_sym(); t_lex_single_sym();
    t_lex_str(); t_lex_char(); t_lex_block_comment(); t_lex_line_comment();
    t_lex_hex_token();
    t_expr_precedence(); t_expr_precedence_rev(); t_expr_left_assoc();
    t_expr_paren(); t_expr_hex_value();
    t_err_unterminated_string(); t_err_bad_number(); t_err_bad_escape();
    t_err_bad_expression();
}

int main(void) {
    run_all();
    UTEST_SUMMARY("minicc_mock");
    return 0;
}