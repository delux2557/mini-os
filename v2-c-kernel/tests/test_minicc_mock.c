/* mini-os/v2-c-kernel/tests/test_minicc_mock.c
 * 任务 4/5 的 Mock 白盒单测驱动（原则 6 的 Mock 半兑现）。
 *
 * 思路（对应 docs/design/minicc-v3-后续任务.md 任务4"Mock 注入缝"）：
 *   - `#define MINICC_MOCK` + `#include "../tools/minicc/minicc.c"` 白盒：直接访问 static CC cc
 *     单例与 next_tok()/expr()/stmt()/fail() 等静态函数。MINICC_MOCK 仅在 `cc` 内追加 fail 的
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
 *   在 test_minicc.sh 宿主层接入，见 [3.5/4] Mock 层。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "utest.h"

#define MINICC_MOCK 1
#include "../tools/minicc/minicc.c"

/* ---- 自建 syscall3 shim（exit/print/fs/brk；内存文件模拟供 minicc_main 全管线） ---- */
static unsigned char arena_mem[64u << 20];
static uint32_t base_brk = 0, cur_brk = 0;

/* 内存文件系统：slot1=输入源码（用例预置 in_fs/in_fs_len），slot2=输出 ELF 产物 */
static unsigned char in_fs[65536]; static int in_fs_len = 0, in_fs_pos = 0;
static unsigned char out_fs[65536]; static int out_fs_len = 0;

int syscall3(int n, int a, int b, int c) {
    switch (n) {
    case 0:  exit((unsigned)a & 255u);                                  /* SYS_EXIT */
    case 1:  fwrite((const void *)(uintptr_t)a, 1, strlen((const char *)(uintptr_t)a), stderr);
             return 0;                                                  /* SYS_PRINT */
    case 13: return 0;                                                  /* SYS_FS_CREATE */
    case 14:                                                            /* SYS_FS_OPEN slot a=b path b=c mode */
        if (a == 1) { in_fs_pos = 0; return 0; }                        /* slot1 = 输入（读） */
        if (a == 2) { out_fs_len = 0; return 0; }                       /* slot2 = 输出（截断写） */
        return -1;
    case 15: {                                                          /* SYS_FS_WRITE slot */
        int cp = (int)c;
        if (cp > 65536 - out_fs_len) cp = 65536 - out_fs_len;
        memcpy(out_fs + out_fs_len, (const void *)(uintptr_t)b, (size_t)cp);
        out_fs_len += cp; return cp;
    }
    case 16: {                                                          /* SYS_FS_READ slot */
        int cp = (int)c;
        if (cp > in_fs_len - in_fs_pos) cp = in_fs_len - in_fs_pos;
        if (cp < 0) cp = 0;
        memcpy((void *)(uintptr_t)b, in_fs + in_fs_pos, (size_t)cp); in_fs_pos += cp; return cp;
    }
    case 17: return 0;                                                  /* SYS_FS_CLOSE */
    case 19: return 0;                                                  /* SYS_FS_DELETE */
    case 35:                                                            /* SYS_BRK */
        if (!base_brk) base_brk = (uint32_t)(uintptr_t)arena_mem;
        if (a == 0) return (int)(cur_brk ? cur_brk : base_brk);
        cur_brk = (uint32_t)a;
        if ((uint32_t)cur_brk - (uint32_t)base_brk > (uint32_t)sizeof arena_mem) exit(2);
        return 0;
    default: return -1;
    }
}

/* 预置输入源码：minicc_main 从 slot1 读它就是源码 */
static void feed_fs_input(const char *s) {
    in_fs_len = (int)strlen(s);
    memcpy(in_fs, s, (size_t)in_fs_len);
    in_fs_pos = 0;
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
/* 顶层程序级预置：除 lex_set 外还须初始化 funcs/gvars 链表——parse_program 末尾
 * `*funcs_tail = fn` 依赖非空 tail 指针；单层 stmt/expr 用例不需要，故仅组6/顶层用。 */
static void prog_set(const char *s) {
    lex_set(s);
    funcs = NULL; funcs_tail = &funcs;
    gvars = NULL; gvars_tail = &gvars;
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
static const char *stmt_err(void) {
    cc.fail_jmp_on = 1;
    if (setjmp(cc.fail_jb) == 0) stmt();
    cc.fail_jmp_on = 0;
    return cc.fail_msg;
}
static const char *prog_err(void) {          /* 顶层程序（函数/全局声明）错误路径 */
    cc.fail_jmp_on = 1;
    if (setjmp(cc.fail_jb) == 0) parse_program();
    cc.fail_jmp_on = 0;
    return cc.fail_msg;
}

/* ---- 断言辅助 ---- */
static void expect_num(Node *n, int v) { CHECK(n != NULL && n->kind == ND_NUM && n->val == v); }
static void expect_sym(Node *n, int k) { CHECK(n != NULL && n->kind == k); }
static int sym_kind(const char *name) { int i = sym_find(name); return i < 0 ? -1 : syms[i].kind; }
static int sym_ty(const char *name)   { int i = sym_find(name); return i < 0 ? -1 : syms[i].ty; }
static int sym_val(const char *name)  { int i = sym_find(name); return i < 0 ? -1 : syms[i].val; }

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

/* ============ V3b：复合赋值 += -= *= /= %= 与前/后缀 ++/--（语法糖） ============ */
/* 先经 stmt() 注册局部 `a`，再换词法源解析表达式（保留符号表，不复位） */
static void t_sugar_decl_a(void) {
    lex_set("int a;");
    next_tok();
    stmt();                             /* 注册 K_LOCAL `a` */
    src = (const unsigned char *)"";    /* 占位，防残留 */
}
static void t_expr_compound_assign(void) {
    t_sugar_decl_a();
    src = (const unsigned char *)"a+=7"; src_len = 4; src_pos = 0; next_tok();
    Node *n = expr();
    expect_sym(n, ND_ASSIGN);           /* lv op= rhs → ND_ASSIGN */
    CHECK(n->l->kind == ND_VAR && n->l == n->r->l);   /* 左值复用（语法糖左值即运算数） */
    expect_sym(n->r, ND_ADD);           /* rhs = lv op rhs */
    expect_num(n->r->r, 7);
}
static void t_expr_prefix_inc(void) {
    t_sugar_decl_a();
    src = (const unsigned char *)"++a"; src_len = 3; src_pos = 0; next_tok();
    Node *n = expr();
    expect_sym(n, ND_ASSIGN);
    expect_sym(n->r, ND_ADD);
    expect_num(n->r->r, 1);             /* ++a → a = a + 1 */
}
static void t_expr_prefix_dec(void) {
    t_sugar_decl_a();
    src = (const unsigned char *)"--a"; src_len = 3; src_pos = 0; next_tok();
    Node *n = expr();
    expect_sym(n, ND_ASSIGN);
    expect_sym(n->r, ND_SUB);
    expect_num(n->r->r, 1);             /* --a → a = a - 1 */
}
static void t_expr_postfix_inc(void) {
    t_sugar_decl_a();
    src = (const unsigned char *)"a++"; src_len = 3; src_pos = 0; next_tok();
    Node *n = expr();
    expect_sym(n, ND_POST_INC);         /* 后缀专用节点：值=旧值 */
    CHECK(n->l->kind == ND_VAR);
    CHECK(n->ty == n->l->ty);
}
static void t_expr_postfix_dec(void) {
    t_sugar_decl_a();
    src = (const unsigned char *)"a--"; src_len = 3; src_pos = 0; next_tok();
    Node *n = expr();
    expect_sym(n, ND_POST_DEC);
    CHECK(n->l->kind == ND_VAR);
}
static void t_err_compound_nonlval(void) {
    t_sugar_decl_a();
    src = (const unsigned char *)"3+=7"; src_len = 4; src_pos = 0; next_tok();
    const char *m = expr_err();
    CHECK(m != NULL && strstr(m, "assign to non-lvalue") != NULL);
}
static void t_err_postfix_nonlval(void) {
    t_sugar_decl_a();
    src = (const unsigned char *)"3++"; src_len = 3; src_pos = 0; next_tok();
    const char *m = expr_err();
    CHECK(m != NULL && strstr(m, "increment/decrement of non-lvalue") != NULL);
}

/* ================= 用例组 3：词法错误路径（setjmp 捕获 fail 消息） ================= */
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

/* ================= 用例组 4：声明分配（成功路径 AST + 符号表 + 帧偏移） ================= */
/* 每个用例以 lex_set 重置（cur_frame=0）。int/char/ptr 标量各占 4 字节帧槽，
 * 数组按 len×元素尺寸紧凑分配，纯字节偏移不保证 4 对齐（V2d）。 */
static void t_decl_int(void) {
    lex_set("int x;");
    next_tok();
    Node *n = stmt();
    expect_sym(n, ND_DECL);
    CHECK(n->ty == TY_INT && n->len == 0 && n->val == 4);   /* 首局部 → 帧偏移 4 */
    CHECK(sym_kind("x") == K_LOCAL && sym_ty("x") == TY_INT && sym_val("x") == 4);
}
static void t_decl_char(void) {
    lex_set("char c;");
    next_tok();
    Node *n = stmt();
    expect_sym(n, ND_DECL);
    CHECK(n->ty == TY_CHAR && n->val == 4);                 /* char 标量仍 4 字节槽 */
    CHECK(sym_ty("c") == TY_CHAR && sym_val("c") == 4);
}
static void t_decl_ptr(void) {
    lex_set("int* p;");
    next_tok();
    Node *n = stmt();
    expect_sym(n, ND_DECL);
    CHECK(n->ty == TY_PTR && n->bty == TY_INT && n->val == 4);
    CHECK(sym_ty("p") == TY_PTR && sym_val("p") == 4);
}
static void t_decl_array_int(void) {
    lex_set("int a[3];");
    next_tok();
    Node *n = stmt();
    expect_sym(n, ND_DECL);
    CHECK(n->ty == TY_ARRAY && n->bty == TY_INT && n->len == 3 && n->val == 12);
    CHECK(sym_ty("a") == TY_ARRAY && sym_val("a") == 12);
}
static void t_decl_array_char(void) {
    lex_set("char s[8];");
    next_tok();
    Node *n = stmt();
    expect_sym(n, ND_DECL);
    CHECK(n->ty == TY_ARRAY && n->bty == TY_CHAR && n->len == 8 && n->val == 8);
    CHECK(sym_ty("s") == TY_ARRAY && sym_val("s") == 8);
}
static void t_decl_init_num(void) {
    lex_set("int x=5;");
    next_tok();
    Node *n = stmt();
    expect_sym(n, ND_DECL);
    CHECK(n->l != NULL && n->l->kind == ND_NUM && n->l->val == 5);  /* 初值 5 */
    CHECK(n->val == 4 && sym_val("x") == 4);
}
static void t_decl_frame_seq(void) {
    /* 连续混合声明验证帧偏移累计（纯字节偏移未对齐 4）：int a=4 + char[3] s=7 + int b=11 */
    lex_set("int a;char s[3];int b;");
    next_tok();
    Node *na = stmt(); CHECK(na->val == 4);
    Node *ns = stmt(); CHECK(ns->ty == TY_ARRAY && ns->val == 7);
    Node *nb = stmt(); CHECK(nb->val == 11);
}

/* ================= 用例组 5：语句错误路径（if/while/for/decl，setjmp 捕获） ================= */
static void t_err_if_missing_lparen(void) {
    lex_set("if 1");
    next_tok();                             /* 预热当前 token='if'，stmt 才能 accept("if") */
    const char *m = stmt_err();                 /* accept(if) 后 expect("(") 失败 */
    CHECK(m != NULL && strstr(m, "expected token") != NULL);
}
static void t_err_if_missing_rparen(void) {
    lex_set("if (1");
    next_tok();
    const char *m = stmt_err();                 /* expr=1 后 expect(")") 失败 */
    CHECK(m != NULL && strstr(m, "expected token") != NULL);
}
static void t_err_while_missing_rparen(void) {
    lex_set("while (1");
    next_tok();
    const char *m = stmt_err();
    CHECK(m != NULL && strstr(m, "expected token") != NULL);
}
static void t_err_for_missing_semicolon(void) {
    lex_set("for (1");
    next_tok();
    const char *m = stmt_err();                 /* init=1 后 expect(";") 失败 */
    CHECK(m != NULL && strstr(m, "expected token") != NULL);
}
static void t_err_for_missing_rparen(void) {
    lex_set("for (;;1");
    next_tok();
    const char *m = stmt_err();                 /* step=1 后 expect(")") 失败 */
    CHECK(m != NULL && strstr(m, "expected token") != NULL);
}
static void t_err_decl_missing_id(void) {
    lex_set("int ;");
    next_tok();
    const char *m = stmt_err();                 /* decl_type 后 !tok_is_word */
    CHECK(m != NULL && strstr(m, "expected identifier") != NULL);
}
static void t_err_decl_missing_semi(void) {
    lex_set("int x");
    next_tok();
    const char *m = stmt_err();                 /* 声明后 expect(";") 失败 */
    CHECK(m != NULL && strstr(m, "expected token") != NULL);
}
static void t_err_array_init_rejected(void) {
    lex_set("int a[3] = 1;");
    next_tok();
    const char *m = stmt_err();                 /* TY_ARRAY && peek("=") */
    CHECK(m != NULL && strstr(m, "array init not supported") != NULL);
}
static void t_err_decl_type_mismatch(void) {
    /* 先成功声明指针 p，再声明 int x = p → 类型不匹配（int 接收指针） */
    lex_set("int* p; int x = p;");
    next_tok();
    stmt();                                     /* decl p 成功，tok 停在 'int' */
    const char *m = stmt_err();                 /* decl x: type_eq(TY_INT, TY_PTR) 失败 */
    CHECK(m != NULL && strstr(m, "type mismatch") != NULL);
}

/* ================= 用例组 5b：循环控制语句 AST 形状（do/break/continue） ================= */
static void t_stmt_do(void) {
    lex_set("do 0; while(1);");
    next_tok();
    Node *n = stmt();
    expect_sym(n, ND_DO);                                   /* do -> ND_DO */
    CHECK(n->b != NULL && n->b->kind == ND_EXPR_STMT);      /* body 为语句 */
    CHECK(n->l != NULL && n->l->kind == ND_NUM);            /* cond 为表达式 */
}
static void t_stmt_break(void) {
    lex_set("break;");
    next_tok();
    expect_sym(stmt(), ND_BREAK);
}
static void t_stmt_continue(void) {
    lex_set("continue;");
    next_tok();
    expect_sym(stmt(), ND_CONTINUE);
}

/* ================= 用例组 6：函数/参数/重定义错误路径（parse_program 层） ================= */
static void t_err_param_name(void) {
    prog_set("int f(int);");                 /* 形参 decl_type 后无名字 */
    next_tok();
    const char *m = prog_err();
    CHECK(m != NULL && strstr(m, "expected parameter name") != NULL);
}
static void t_err_array_param(void) {
    prog_set("int g(int a[3]){}");           /* 数组参数不支持 */
    next_tok();
    const char *m = prog_err();
    CHECK(m != NULL && strstr(m, "unsupported: array parameter") != NULL);
}
static void t_err_func_body(void) {
    prog_set("int h() ;");                    /* 缺函数体 { */
    next_tok();
    const char *m = prog_err();
    CHECK(m != NULL && strstr(m, "expected function body") != NULL);
}
static void t_err_redefined_func(void) {
    /* 函数先定义，同名再声明为全局变量 → K_FUNC↔K_GLOBAL 冲突，parse 层报 redefined。
     * 注：minicc 的函数-函数两次定义 parse 期不报（val 仍 -1），last-definition 覆盖，属实测允许行为。 */
    prog_set("int f(){} int f;");
    next_tok();
    const char *m = prog_err();
    CHECK(m != NULL && strstr(m, "redefined") != NULL);
}
static void t_err_redefined_global(void) {
    prog_set("int g; int g;");                /* 全局变量重复定义 */
    next_tok();
    const char *m = prog_err();
    CHECK(m != NULL && strstr(m, "redefined") != NULL);
}

/* ================= 用例组 7：minicc_main 全管线成功路径（编译成功 + ELF 产物落盘） ================= */
/* 经内存文件系统跑完整编译：读源码 → 建树 → 代码生成 → 写 ELF 到 out end_fs；断言返回 0 且
 * 产物以 ELF magic 开头、长度等于 code_len。这测的是整条编译器管线而非单层。 */
static void t_pipeline_simple(void) {
    t_reset();
    feed_fs_input("int main(){return 42;}");
    char *av[3]; av[0] = "/"; av[1] = "/p.c"; av[2] = "/p.elf";
    int rc = minicc_main((char *)av, 3);
    CHECK(rc == 0);
    CHECK(out_fs_len > 95 && out_fs[0] == 0x7f && out_fs[1] == 'E' &&
          out_fs[2] == 'L' && out_fs[3] == 'F');   /* ELF32 magic */
    CHECK(code_len == out_fs_len);                 /* 产物字节 = 写入字节 */
}
static void t_pipeline_full(void) {
    /* 大而全：全局 vars + 递归 + for + 数组 + 指针 + 字符串，过整条编译 */
    t_reset();
    feed_fs_input("int g;int main(){int a[3];int* p;p=&a[0];int i;"
                  "for(i=0;i<3;i=i+1)a[i]=i;i=0;g=0;"
                  "while(i<3){g=g+a[i];i=i+1;}return g;}");
    char *av[3]; av[0] = "/"; av[1] = "/pf.c"; av[2] = "/pf.elf";
    int rc = minicc_main((char *)av, 3);
    CHECK(rc == 0);
    CHECK(out_fs_len > 95 && out_fs[0] == 0x7f && out_fs[1] == 'E' && out_fs[2] == 'L');
    CHECK(code_len == out_fs_len);
}

static void t_pipeline_sugar(void) {
    /* V3b：复合赋值 + 前/后缀 ++/-- 走整条编译（含 codegen，防坏节点） */
    t_reset();
    feed_fs_input("int main(){int a;int s;a=5;a+=7;a-=3;a*=4;a/=2;a%=7;"
                  "int c;c=++a;c=a++;c=a--;c=--a;return c;}");
    char *av[3]; av[0] = "/"; av[1] = "/ps.c"; av[2] = "/ps.elf";
    int rc = minicc_main((char *)av, 3);
    CHECK(rc == 0);
    CHECK(out_fs_len > 95 && out_fs[0] == 0x7f && out_fs[1] == 'E' && out_fs[2] == 'L');
    CHECK(code_len == out_fs_len);
}

static void run_all(void) {
    t_lex_num(); t_lex_word(); t_lex_double_sym(); t_lex_single_sym();
    t_lex_str(); t_lex_char(); t_lex_block_comment(); t_lex_line_comment();
    t_lex_hex_token();
    t_expr_precedence(); t_expr_precedence_rev(); t_expr_left_assoc();
    t_expr_paren(); t_expr_hex_value();
    t_expr_compound_assign(); t_expr_prefix_inc(); t_expr_prefix_dec();
    t_expr_postfix_inc(); t_expr_postfix_dec();
    t_err_compound_nonlval(); t_err_postfix_nonlval();
    t_err_unterminated_string(); t_err_bad_number(); t_err_bad_escape();
    t_err_bad_expression();
    t_decl_int(); t_decl_char(); t_decl_ptr(); t_decl_array_int(); t_decl_array_char();
    t_decl_init_num(); t_decl_frame_seq();
    t_err_if_missing_lparen(); t_err_if_missing_rparen(); t_err_while_missing_rparen();
    t_err_for_missing_semicolon(); t_err_for_missing_rparen();
    t_stmt_do(); t_stmt_break(); t_stmt_continue();
    t_err_decl_missing_id(); t_err_decl_missing_semi(); t_err_array_init_rejected();
    t_err_decl_type_mismatch();
    t_err_param_name(); t_err_array_param(); t_err_func_body();
    t_err_redefined_func(); t_err_redefined_global();
    t_pipeline_simple(); t_pipeline_full(); t_pipeline_sugar();
}

int main(void) {
    run_all();
    UTEST_SUMMARY("minicc_mock");
    return 0;
}