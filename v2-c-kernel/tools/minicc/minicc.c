/* mini-os/v2-c-kernel/tools/minicc/minicc.c
 * minicc —— mini-os 自研小型 C 编译器（V2a：AST 引入，int-only 语义与 V1 等价）。
 *
 * 版权与许可：
 *   Copyright (C) 2026 mini-os authors
 *   SPDX-License-Identifier: MIT
 *   本项目（mini-os）整体为 MIT 许可。cc500 为 GPL 工具链组件，minicc 是
 *   全新的独立实现（MIT），用于逐步取代 cc500 作为 mini-os 的板载编译器。
 *
 * 架构（详见 docs/design/minicc-design.md）：
 *   V1 为单遍直接生成（无 AST）；V2a 起引入显式 AST：Parser 建树、Codegen 遍历。
 *   设计决策 4.3：解引用/复合类型需要"先解析后决定生成策略"，故在本版本引入 AST，
 *   为 V2b 指针/解引用与语义分析（Mock 单测）打基础。int-only 语义与 V1 完全等价，
 *   make test-minicc 应保持全绿（本版是纯结构重构 + 测试锁定）。
 *
 * 当前子集（int-only，与 V1 相同）：
 *   类型 int；字面量十进制；局部/参数/全局变量（全局仅常量初始化）；
 *   + - * / % < <= > >= == != && || ! 一元负号、赋值；{} if/else while return 表达式语句；
 *   多参数函数、递归、前向调用；块注释与 // 行注释。
 *
 * 代码生成（x86 32 位，直出 ELF32）：
 *   - 标准 ebp 栈帧；表达式值在 eax；二元运算 push 左操作数。
 *   - 调用约定（自洽）：参数从左到右求值逐个 push；第 i 个形参位于 [ebp+8+4*(n-1-i)]。
 *   - ELF 布局：0x00 头+程序头，0x54 入口 stub
 *       call main ; mov %eax,%ebx ; xor %eax,%eax ; int $0x80
 *     （main 返回值作 sys_exit 退出码：mini-os ABI eax=号 ebx=a 返回 ebx）。
 *
 * 运行环境：guest 由 minicc_crt.c 提供 syscall3（int $0x80）；host 由
 *   tools/minicc/host_crt.c 提供（Linux 文件模拟）。仅依赖 syscall3(0/1/13/14/15/16/17/19/35)。
 */

#include <stdint.h>
#include <stddef.h>

/* ================= 系统调用与运行时基础 ================= */

int syscall3(int n, int a, int b, int c);   /* 由 CRT 提供 */

static void sys_print(const char *s) {
    syscall3(1, (int)s, 0, 0);              /* SYS_PRINT */
}

const char *tokbuf_current(void);       /* 定义在词法章节 */

/* 编译错误：打印上下文 token 后以 1 退出（host/guest 均由 sys_exit 兜底） */
static void fail(const char *msg) {
    sys_print("minicc: error: ");
    sys_print(msg);
    sys_print(" [");
    sys_print(tokbuf_current());
    sys_print("]\n");
    syscall3(0, 1, 0, 0);                   /* SYS_EXIT(1) */
}

/* ---- 无 libc 基础函数（guest -nostdlib 下可用） ---- */
static int s_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void s_cpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = 0;
}

/* brk 线性分配（guest 走 SYS_BRK 移动 program break；host shim 用静态竞技场模拟）。
 * 编译期间不做任何释放——整棵 AST/符号/代码缓冲即"竞技场"（设计文档第 5 节）。 */
static void *xmalloc(int n) {
    uint32_t old = (uint32_t)syscall3(35, 0, 0, 0);          /* SYS_BRK 查询 */
    if (syscall3(35, (int)(old + (uint32_t)n), 0, 0) != 0) { /* SYS_BRK 上移 */
        sys_print("minicc: out of memory\n");
        syscall3(0, 1, 0, 0);
    }
    return (void *)(uint32_t)old;
}

/* ================= 输出码流缓冲 ================= */

static unsigned char *code;
static int code_len;
static int code_cap;

static void emit1(int b) {
    if (code_len >= code_cap) {
        unsigned char *n = (unsigned char *)xmalloc(code_cap * 2);
        for (int i = 0; i < code_len; i++) n[i] = code[i];
        code = n;
        code_cap *= 2;
    }
    code[code_len++] = (unsigned char)b;
}

static void emit4(int v) {
    emit1(v & 0xff); emit1((v >> 8) & 0xff);
    emit1((v >> 16) & 0xff); emit1((v >> 24) & 0xff);
}

static void emit_op(const char *s) {
    while (*s) emit1((unsigned char)*s++);
}

static void save32(int pos, int v) {
    code[pos]     = (unsigned char)(v & 0xff);
    code[pos + 1] = (unsigned char)((v >> 8) & 0xff);
    code[pos + 2] = (unsigned char)((v >> 16) & 0xff);
    code[pos + 3] = (unsigned char)((v >> 24) & 0xff);
}

/* ================= 符号表 ================= */

#define SYM_MAX 512
enum { K_FUNC, K_GLOBAL, K_LOCAL, K_ARG };
enum { TY_INT, TY_PTR };        /* V2b：int 与 int*（多级指针不支持） */

typedef struct {
    char name[32];
    int kind;
    int ty;                     /* TY_INT / TY_PTR */
    int val;                    /* FUNC: 代码偏移(未定义=-1)；GLOBAL: 数据偏移；
                                   LOCAL: 局部序号；ARG: 参数序号 */
} Sym;

static Sym syms[SYM_MAX];
static int nsym;

static int sym_find(const char *name) {
    for (int i = 0; i < nsym; i++)
        if (s_eq(syms[i].name, name)) return i;
    return -1;
}

static int sym_add(const char *name, int kind, int ty, int val) {
    if (nsym >= SYM_MAX) fail("symbol table full");
    s_cpy(syms[nsym].name, name);
    syms[nsym].kind = kind;
    syms[nsym].ty = ty;
    syms[nsym].val = val;
    return nsym++;
}

/* ================= 补丁（符号引用 / 控制流标签） ================= */

#define PATCH_MAX 1024
enum { P_CALL, P_ADDR };
typedef struct { char name[32]; int pos; int kind; } Patch;
static Patch patches[PATCH_MAX];
static int npatch;

static void patch_add(const char *name, int pos, int kind) {
    if (npatch >= PATCH_MAX) fail("too many references");
    s_cpy(patches[npatch].name, name);
    patches[npatch].pos = pos;
    patches[npatch].kind = kind;
    npatch++;
}

#define LAB_MAX 4096
enum { L_COND, L_JMP };
typedef struct { int pos; int kind; int target; } Lab;
static Lab labs[LAB_MAX];
static int nlab;

static int new_lab(void) {
    if (nlab >= LAB_MAX) fail("too many labels");
    return nlab++;
}

static void emit_cond(int op, int lab) {
    labs[lab].pos = code_len;
    labs[lab].kind = L_COND;
    emit1(0x0F); emit1(op); emit4(0);
}

static void emit_jmp(int lab) {
    labs[lab].pos = code_len;
    labs[lab].kind = L_JMP;
    emit1(0xE9); emit4(0);
}

static void emit_jmp_to(int target) {
    int p = code_len;
    emit1(0xE9);
    emit4(target - (p + 5));
}

static void patch_lab(int lab, int target) {
    labs[lab].target = target;
}

/* ================= AST ================= */

enum {
    ND_NUM, ND_VAR, ND_FUNCALL,
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD,
    ND_EQ, ND_NE, ND_LT, ND_LE, ND_GT, ND_GE,
    ND_AND, ND_OR, ND_NEG, ND_NOT, ND_ASSIGN,
    ND_ADDR, ND_DEREF,         /* V2b：& 取地址 / * 解引用 */
    ND_EXPR_STMT, ND_BLOCK, ND_IF, ND_WHILE, ND_RET,
    ND_DECL, ND_FUNC, ND_GVAR
};

typedef struct Node Node;
struct Node {
    int kind;
    int ty;             /* TY_INT / TY_PTR：表达式类型（VAR/DEREF/ADD 等） */
    Node *l, *r;        /* 二元操作数；ASSIGN: l=左值 r=右值；IF: l=cond r=then；
                           WHILE: l=cond r=body；RET: l=表达式；NOT/NEG/ADDR/DEREF: l=操作数 */
    Node *a, *b;        /* FUNCALL: a=实参链表；FUNC: a=形参链表 b=函数体；
                           IF: b=else 分支（可空）；BLOCK: a=语句链表 */
    Node *next;         /* 语句/实参/形参链表的后继 */
    int val;            /* NUM: 数值；DECL: 局部槽位；FUNC/GVAR: 符号下标；VAR: 无 */
    int vkind, vslot;   /* VAR: 变量类别（K_LOCAL/K_ARG/K_GLOBAL）与槽位序号（local/arg）——
                           节点脱离符号表自携带，作用域恢复丢弃符号后仍可正确生成 */
    char vname[32];     /* VAR: 全局变量名（patch 用） */
    int ival;           /* GVAR: 常量初始化值（无初始化=0） */
    int nargs, nlocals; /* FUNC: 形参个数 / 局部变量总数（帧大小=4*nlocals）；FUNCALL: 实参个数 */
    char name[32];      /* FUNC/GVAR/FUNCALL: 名字 */
};

static Node *node_new(int kind) {
    Node *n = (Node *)xmalloc(sizeof(Node));
    n->kind = kind;
    n->ty = TY_INT;
    n->l = n->r = n->a = n->b = n->next = NULL;
    n->val = n->vkind = n->vslot = n->ival = n->nargs = n->nlocals = 0;
    n->vname[0] = 0;
    n->name[0] = 0;
    return n;
}

/* ================= 代码生成辅助 ================= */

#define CODE_BASE 0x800A0000u       /* APP_LINK：与内核 ELF 加载器一致 */

static int cur_nargs;               /* 当前函数形参个数（生成 arg 地址用） */
static int frame_patch;             /* prologue `sub esp,imm32` 的 imm 位置 */

static void emit_mov_imm(int v) { emit1(0xB8); emit4(v); }
static void emit_lea_ebp(int disp) { emit_op("\x8d\x85"); emit4(disp); }
static void emit_load(void) { emit1(0x8B); emit1(0x00); }        /* mov (%eax),%eax */
static void emit_store(void) { emit_op("\x5b\x89\x03"); }        /* pop %ebx; mov %eax,(%ebx) */
static void emit_test(void) { emit_op("\x85\xc0"); }
static void emit_epilogue(void) { emit_op("\x89\xec\x5d\xc3"); } /* mov %esp,%ebp; pop %ebp; ret */

static void emit_add_esp(int n4) {
    if (n4 <= 127) { emit_op("\x83\xc4"); emit1(n4); }
    else           { emit_op("\x81\xc4"); emit4(n4); }
}

/* ================= 词法 ================= */

#define TOK_MAX 64
static const unsigned char *src;
static int src_len;
static int src_pos;
static char tok[TOK_MAX];
static int tok_is_word;
static int tok_is_num;

static int peekc(void)  { return src_pos < src_len ? src[src_pos] : -1; }
static int peekc2(void) { return src_pos + 1 < src_len ? src[src_pos + 1] : -1; }

const char *tokbuf_current(void) { return tok; }

static void next_tok(void) {
    for (;;) {
        int c = peekc();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { src_pos++; continue; }
        if (c == '/' && peekc2() == '/') {
            src_pos += 2;
            while (peekc() != '\n' && peekc() >= 0) src_pos++;
            continue;
        }
        if (c == '/' && peekc2() == '*') {
            src_pos += 2;
            for (;;) {
                if (peekc() < 0) fail("unterminated comment");
                if (peekc() == '*' && peekc2() == '/') { src_pos += 2; break; }
                src_pos++;
            }
            continue;
        }
        break;
    }
    if (peekc() < 0) { tok[0] = 0; tok_is_word = 0; tok_is_num = 0; return; }

    int c = src[src_pos++];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int n = 0;
        tok[n++] = (char)c;
        while (n < TOK_MAX - 1) {
            int d = peekc();
            if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                (d >= '0' && d <= '9') || d == '_') { tok[n++] = (char)d; src_pos++; }
            else break;
        }
        tok[n] = 0;
        tok_is_word = 1; tok_is_num = 0;
        return;
    }
    if (c >= '0' && c <= '9') {
        int n = 0;
        tok[n++] = (char)c;
        while (n < TOK_MAX - 1) {
            int d = peekc();
            if (d >= '0' && d <= '9') { tok[n++] = (char)d; src_pos++; }
            else break;
        }
        tok[n] = 0;
        if (peekc() == '_' || (peekc() >= 'a' && peekc() <= 'z') ||
            (peekc() >= 'A' && peekc() <= 'Z'))
            fail("bad number");
        tok_is_word = 0; tok_is_num = 1;
        return;
    }
    int d = peekc();
    if ((c == '=' && d == '=') || (c == '!' && d == '=') ||
        (c == '<' && d == '=') || (c == '>' && d == '=') ||
        (c == '&' && d == '&') || (c == '|' && d == '|')) {
        tok[0] = (char)c; tok[1] = (char)d; tok[2] = 0;
        src_pos++;
        tok_is_word = 0; tok_is_num = 0;
        return;
    }
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
        c == '<' || c == '>' || c == '=' || c == '!' ||
        c == '(' || c == ')' || c == '{' || c == '}' || c == ';' || c == ',') {
        tok[0] = (char)c; tok[1] = 0;
        tok_is_word = 0; tok_is_num = 0;
        return;
    }
    tok[0] = (char)c; tok[1] = 0;
    tok_is_word = 0; tok_is_num = 0;
}

static int peek(const char *s) { return s_eq(s, tok); }
static int accept(const char *s) {
    if (peek(s)) { next_tok(); return 1; }
    return 0;
}
static void expect(const char *s) {
    if (!accept(s)) fail("expected token");
}

/* 读取类型声明中 `int` 之后的 `*` 数量（int 已被入口 accept 消费）；
 * 返回 TY_INT / TY_PTR（V2b 不支持多级指针） */
static int type_after_int(void) {
    int stars = 0;
    while (accept("*")) stars++;
    if (stars > 1) fail("unsupported: multi-level pointer");
    return stars ? TY_PTR : TY_INT;
}

/* ================= 语法分析（建树） ================= */

static Node *expr(void);

static Node *primary(void) {
    Node *n;
    if (tok_is_num) {
        n = node_new(ND_NUM);
        n->val = 0;
        for (int i = 0; tok[i]; i++) {
            if (tok[i] < '0' || tok[i] > '9') fail("bad number");
            n->val = n->val * 10 + (tok[i] - '0');
        }
        next_tok();
        return n;
    }
    if (tok_is_word) {
        char name[32];
        s_cpy(name, tok);
        next_tok();
        if (peek("(")) {                        /* 函数调用 */
            n = node_new(ND_FUNCALL);
            s_cpy(n->name, name);
            int si = sym_find(name);
            if (si >= 0 && syms[si].kind != K_FUNC) fail("call to non-function");
            if (si < 0) sym_add(name, K_FUNC, TY_INT, -1);   /* 隐式声明 */
            n->val = si < 0 ? nsym - 1 : si;         /* 符号下标 */
            expect("(");
            Node *head = NULL, **tail = &head;
            while (!peek(")")) {
                Node *arg = expr();
                *tail = arg; tail = &arg->next;
                n->nargs++;
                if (!accept(",")) break;
            }
            expect(")");
            n->a = head;
            return n;
        }
        int si = sym_find(name);
        if (si < 0) fail("undefined variable");
        n = node_new(ND_VAR);
        n->ty = syms[si].ty;
        n->vkind = syms[si].kind;
        n->vslot = syms[si].val;
        if (n->vkind == K_GLOBAL) s_cpy(n->vname, syms[si].name);
        return n;
    }
    if (accept("(")) {
        n = expr();
        expect(")");
        return n;
    }
    fail("bad expression");
    return NULL;
}

static Node *unary(void) {
    if (accept("-")) {
        Node *n = node_new(ND_NEG);
        n->l = unary();
        return n;
    }
    if (accept("!")) {
        Node *n = node_new(ND_NOT);
        n->l = unary();
        return n;
    }
    if (accept("&")) {
        Node *n = node_new(ND_ADDR);
        n->l = unary();
        /* & 操作数必须可寻址（变量/解引用）；类型为指向其的指针 */
        if (n->l->kind != ND_VAR && n->l->kind != ND_DEREF) fail("cannot take address");
        n->ty = TY_PTR;
        return n;
    }
    if (accept("*")) {
        Node *n = node_new(ND_DEREF);
        n->l = unary();
        if (n->l->ty != TY_PTR) fail("dereference of non-pointer");
        n->ty = TY_INT;
        return n;
    }
    return primary();
}

static Node *bin(Node *l, Node *r, int kind) {
    Node *n = node_new(kind);
    n->l = l;
    n->r = r;
    /* 指针算术只对 +/- 传播指针类型；其余运算结果为 int */
    if ((kind == ND_ADD || kind == ND_SUB) &&
        (l->ty == TY_PTR || r->ty == TY_PTR))
        n->ty = TY_PTR;
    return n;
}

static Node *mul(void) {
    Node *n = unary();
    for (;;) {
        if (accept("*"))      n = bin(n, unary(), ND_MUL);
        else if (accept("/")) n = bin(n, unary(), ND_DIV);
        else if (accept("%")) n = bin(n, unary(), ND_MOD);
        else return n;
    }
}

static Node *add(void) {
    Node *n = mul();
    for (;;) {
        if (accept("+"))      n = bin(n, mul(), ND_ADD);
        else if (accept("-")) n = bin(n, mul(), ND_SUB);
        else return n;
    }
}

static Node *rel(void) {
    Node *n = add();
    for (;;) {
        if (accept("<"))       n = bin(n, add(), ND_LT);
        else if (accept("<=")) n = bin(n, add(), ND_LE);
        else if (accept(">"))  n = bin(n, add(), ND_GT);
        else if (accept(">=")) n = bin(n, add(), ND_GE);
        else return n;
    }
}

static Node *eq(void) {
    Node *n = rel();
    for (;;) {
        if (accept("==")) n = bin(n, rel(), ND_EQ);
        else if (accept("!=")) n = bin(n, rel(), ND_NE);
        else return n;
    }
}

static Node *land(void) {
    Node *n = eq();
    while (accept("&&")) n = bin(n, eq(), ND_AND);
    return n;
}

static Node *lor(void) {
    Node *n = land();
    while (accept("||")) n = bin(n, land(), ND_OR);
    return n;
}

static Node *expr(void) {
    Node *n = lor();
    if (accept("=")) {
        Node *a = node_new(ND_ASSIGN);
        a->l = n;               /* 左值（语法层不校验，codegen 时对非地址报错） */
        a->r = expr();          /* 右结合 */
        if (a->l->kind != ND_VAR && a->l->kind != ND_DEREF) fail("assign to non-lvalue");
        if (a->l->ty != a->r->ty) fail("type mismatch in assignment");
        return a;
    }
    return n;
}

/* ---- 语句 ---- */

static int cur_nlocals;         /* 当前函数局部变量计数（帧槽位分配，块间不回收） */

static Node *stmt(void);

static Node *block_stmt(void) {
    int mark = nsym;
    Node *head = NULL, **tail = &head;
    while (!peek("}")) {
        if (tok[0] == 0) fail("unexpected end of file");
        Node *s = stmt();
        *tail = s; tail = &s->next;
    }
    expect("}");
    nsym = mark;                /* 作用域：丢弃块内局部符号 */
    Node *n = node_new(ND_BLOCK);
    n->a = head;
    return n;
}

static Node *stmt(void) {
    Node *n;
    if (peek("{")) {
        next_tok();
        return block_stmt();
    }
    if (accept("int")) {
        /* 局部声明（`int x;` / `int* p;` / `int *p;`） */
        int ty = type_after_int();
        if (!tok_is_word) fail("expected identifier");
        char name[32];
        s_cpy(name, tok);
        next_tok();
        if (cur_nlocals >= 512) fail("too many locals");
        n = node_new(ND_DECL);
        n->ty = ty;
        n->val = cur_nlocals++;
        sym_add(name, K_LOCAL, ty, n->val);
        if (accept("=")) {
            n->l = expr();
            if (n->ty != n->l->ty) fail("type mismatch in initialization");
        }
        expect(";");
        return n;
    }
    if (accept("if")) {
        expect("(");
        n = node_new(ND_IF);
        n->l = expr();
        expect(")");
        n->r = stmt();
        if (accept("else")) n->b = stmt();
        return n;
    }
    if (accept("while")) {
        expect("(");
        n = node_new(ND_WHILE);
        n->l = expr();
        expect(")");
        n->r = stmt();
        return n;
    }
    if (accept("return")) {
        n = node_new(ND_RET);
        if (!peek(";")) n->l = expr();
        expect(";");
        return n;
    }
    n = node_new(ND_EXPR_STMT);
    n->l = expr();
    expect(";");
    return n;
}

/* ---- 程序（全局声明 + 函数定义） ---- */

static Node *funcs;             /* 函数节点链表（codegen 顺序） */
static Node **funcs_tail;
static Node *gvars;             /* 全局变量节点链表 */
static Node **gvars_tail;

static void parse_program(void) {
    for (;;) {
        if (tok[0] == 0) return;
        if (!accept("int")) fail("expected 'int'");
        int ty = type_after_int();
        if (!tok_is_word) fail("expected identifier");
        char name[32];
        s_cpy(name, tok);
        next_tok();
        if (accept("(")) {
            /* ---- 函数定义 ---- */
            int si = sym_find(name);
            if (si >= 0) {
                if (syms[si].kind != K_FUNC || syms[si].val >= 0) fail("redefined");
            } else {
                si = sym_add(name, K_FUNC, TY_INT, -1);
            }
            Node *fn = node_new(ND_FUNC);
            s_cpy(fn->name, name);
            fn->val = si;
            int func_scope = nsym;
            cur_nargs = 0; cur_nlocals = 0;
            Node *params = NULL, **ptail = &params;
            if (!peek(")")) {
                for (;;) {
                    if (!accept("int")) fail("expected 'int' in parameters");
                    int pty = type_after_int();
                    if (!tok_is_word) fail("expected parameter name");
                    Node *p = node_new(ND_VAR);
                    s_cpy(p->name, tok);
                    next_tok();
                    p->ty = pty;
                    p->vkind = K_ARG;
                    p->vslot = cur_nargs;
                    sym_add(p->name, K_ARG, pty, cur_nargs);
                    *ptail = p; ptail = &p->next;
                    cur_nargs++;
                    if (!accept(",")) break;
                }
            }
            expect(")");
            fn->nargs = cur_nargs;
            fn->a = params;
            if (!accept("{")) fail("expected function body");
            fn->b = block_stmt();
            fn->nlocals = cur_nlocals;
            nsym = func_scope;
            *funcs_tail = fn; funcs_tail = &fn->next;
        } else {
            /* ---- 全局变量 ---- */
            if (sym_find(name) >= 0) fail("redefined");
            Node *g = node_new(ND_GVAR);
            s_cpy(g->name, name);
            int si = sym_add(name, K_GLOBAL, ty, 0);
            g->val = si;
            if (accept("=")) {
                if (!tok_is_num) fail("global init must be a constant");
                g->ival = 0;
                for (int i = 0; tok[i]; i++) g->ival = g->ival * 10 + (tok[i] - '0');
                next_tok();
            }
            expect(";");
            *gvars_tail = g; gvars_tail = &g->next;
        }
    }
}

/* ================= 代码生成（遍历 AST） ================= */

static void gen(Node *n);       /* gen_addr 在解引用时递归取指针值 */

static void gen_addr(Node *n) {
    if (n->kind == ND_VAR) {
        if (n->vkind == K_LOCAL)      emit_lea_ebp(-4 * (n->vslot + 1));
        else if (n->vkind == K_ARG)   emit_lea_ebp(8 + 4 * (cur_nargs - 1 - n->vslot));
        else if (n->vkind == K_GLOBAL) {
            emit_mov_imm(0);
            patch_add(n->vname, code_len - 4, P_ADDR);
        } else fail("not a variable");
        return;
    }
    if (n->kind == ND_DEREF) {
        gen(n->l);                  /* 解引用：地址 = 指针值（在 eax） */
        return;
    }
    fail("assign to non-lvalue");
}

static void gen(Node *n) {
    switch (n->kind) {
    case ND_NUM: emit_mov_imm(n->val); return;
    case ND_VAR:
    case ND_DEREF:
        gen_addr(n); emit_load(); return;
    case ND_ADDR:
        gen_addr(n->l); return;     /* &x：值 = x 的地址 */
    case ND_NEG: gen(n->l); emit_op("\xf7\xd8"); return;
    case ND_NOT:
        gen(n->l); emit_test();
        emit_op("\x0f\x94\xc0\x0f\xb6\xc0");    /* sete al; movzbl al,eax */
        return;
    case ND_ASSIGN:
        gen_addr(n->l); emit1(0x50);
        gen(n->r);
        emit_store();
        return;
    case ND_ADD:
        gen(n->l); emit1(0x50); gen(n->r);
        /* 指针算术：int 侧按元素尺寸（4 字节）缩放 */
        if (n->l->ty == TY_PTR && n->r->ty != TY_PTR) emit_op("\xc1\xe0\x02");   /* shl $2,%eax */
        else if (n->l->ty != TY_PTR && n->r->ty == TY_PTR) emit_op("\xc1\xe3\x02"); /* shl $2,%ebx */
        emit_op("\x5b\x01\xd8");
        return;
    case ND_SUB:
        gen(n->l); emit1(0x50); gen(n->r);
        if (n->l->ty == TY_PTR && n->r->ty != TY_PTR) emit_op("\xc1\xe0\x02");
        else if (n->r->ty == TY_PTR) fail("invalid pointer subtraction");
        emit_op("\x5b\x29\xc3\x89\xd8");
        return;
    case ND_MUL: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x0f\xaf\xc3"); return;
    case ND_DIV: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x87\xd8\x99\xf7\xfb"); return;
    case ND_MOD: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x87\xd8\x99\xf7\xfb\x89\xd0"); return;
    case ND_LT: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x39\xc3\x0f\x9c\xc0\x0f\xb6\xc0"); return;
    case ND_LE: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x39\xc3\x0f\x9e\xc0\x0f\xb6\xc0"); return;
    case ND_GT: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x39\xc3\x0f\x9f\xc0\x0f\xb6\xc0"); return;
    case ND_GE: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x39\xc3\x0f\x9d\xc0\x0f\xb6\xc0"); return;
    case ND_EQ: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x39\xc3\x0f\x94\xc0\x0f\xb6\xc0"); return;
    case ND_NE: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x39\xc3\x0f\x95\xc0\x0f\xb6\xc0"); return;
    case ND_AND: {
        /* 短路 &&：两个操作数各一次 test+jz（补丁表每条记录只回填一处，须独立标签） */
        int fa1 = new_lab(), fa2 = new_lab(), en = new_lab();
        gen(n->l); emit_test(); emit_cond(0x84, fa1);
        gen(n->r); emit_test(); emit_cond(0x84, fa2);
        emit_mov_imm(1);
        emit_jmp(en);
        patch_lab(fa1, code_len);
        patch_lab(fa2, code_len);
        emit_op("\x31\xc0");
        patch_lab(en, code_len);
        return;
    }
    case ND_OR: {
        int tr1 = new_lab(), tr2 = new_lab(), en = new_lab();
        gen(n->l); emit_test(); emit_cond(0x85, tr1);
        gen(n->r); emit_test(); emit_cond(0x85, tr2);
        emit_op("\x31\xc0");
        emit_jmp(en);
        patch_lab(tr1, code_len);
        patch_lab(tr2, code_len);
        emit_mov_imm(1);
        patch_lab(en, code_len);
        return;
    }
    case ND_FUNCALL: {
        for (Node *a = n->a; a; a = a->next) {
            gen(a);
            emit1(0x50);                /* 参数从左到右逐个 push */
        }
        emit1(0xE8); emit4(0);
        patch_add(n->name, code_len - 4, P_CALL);
        emit_add_esp(n->nargs * 4);
        return;
    }
    default:
        fail("internal: bad expr node");
    }
}

static void gen_stmt(Node *n) {
    switch (n->kind) {
    case ND_EXPR_STMT: gen(n->l); return;
    case ND_BLOCK:
        for (Node *s = n->a; s; s = s->next) gen_stmt(s);
        return;
    case ND_DECL:
        if (n->l) {
            emit_lea_ebp(-4 * (n->val + 1));    /* 槽位地址（ND_DECL 直接算，不经 gen_addr） */
            emit1(0x50);
            gen(n->l);
            emit_store();
        }
        return;
    case ND_IF: {
        int els = new_lab(), en = new_lab();
        gen(n->l); emit_test(); emit_cond(0x84, els);
        gen_stmt(n->r);
        emit_jmp(en);
        patch_lab(els, code_len);
        if (n->b) gen_stmt(n->b);
        patch_lab(en, code_len);
        return;
    }
    case ND_WHILE: {
        int top = code_len;
        int en = new_lab();
        gen(n->l); emit_test(); emit_cond(0x84, en);
        gen_stmt(n->r);
        emit_jmp_to(top);
        patch_lab(en, code_len);
        return;
    }
    case ND_RET:
        if (n->l) gen(n->l);
        emit_epilogue();
        return;
    default:
        fail("internal: bad stmt node");
    }
}

/* 全局数据（ND_GVAR）：emit 4 字节占位，回写初值，登记符号偏移 */
static void gen_global(Node *n) {
    int si = n->val;
    int pos = code_len;
    emit4(0);
    if (n->ival) save32(pos, n->ival);      /* 常量初始化 */
    syms[si].val = pos;
}

/* 函数：prologue + 体 + epilogue + 帧大小回填 */
static void gen_func(Node *n) {
    int si = n->val;
    syms[si].val = code_len;
    cur_nargs = n->nargs;
    emit_op("\x55\x89\xe5");                /* push %ebp; mov %esp,%ebp */
    emit_op("\x81\xec"); emit4(0);          /* sub $0,%esp（帧大小收尾回填） */
    frame_patch = code_len - 4;
    gen_stmt(n->b);
    emit_epilogue();
    save32(frame_patch, n->nlocals * 4);
}

/* ================= 收尾：回填补丁、校验、写文件 ================= */

static void finish(void) {
    for (int i = 0; i < nlab; i++) {
        int imm = labs[i].pos + (labs[i].kind == L_COND ? 2 : 1);
        int rel = labs[i].target - (labs[i].pos + (labs[i].kind == L_COND ? 6 : 5));
        save32(imm, rel);
    }
    for (int i = 0; i < npatch; i++) {
        int si = sym_find(patches[i].name);
        if (patches[i].kind == P_CALL) {
            if (si < 0 || syms[si].kind != K_FUNC || syms[si].val < 0)
                fail("undefined function");
            save32(patches[i].pos, syms[si].val - (patches[i].pos + 4));
        } else {
            if (si < 0 || syms[si].kind != K_GLOBAL) fail("internal: bad addr patch");
            save32(patches[i].pos, (int)(CODE_BASE + (uint32_t)syms[si].val));
        }
    }
    save32(68, code_len);                   /* p_filesz */
    save32(72, code_len);                   /* p_memsz */
}

/* ================= mini-os 文件系统 I/O =================
 * SYS_PRINT=1 SYS_FS_CREATE=13 SYS_FS_OPEN=14 SYS_FS_WRITE=15 SYS_FS_READ=16
 * SYS_FS_CLOSE=17 SYS_FS_DELETE=19 SYS_BRK=35 SYS_EXIT=0 */

static unsigned char *in_data;
static int in_len;

static int open_input(const char *path) {
    if (syscall3(14, 1, (int)path, 0) != 0) return -1;
    in_data = (unsigned char *)xmalloc(65536);
    in_len = 0;
    for (;;) {
        if (in_len >= 65536) {
            syscall3(17, 1, 0, 0);
            sys_print("minicc: input too big (>64KB)\n");
            syscall3(0, 1, 0, 0);
        }
        int n = syscall3(16, 1, (int)(in_data + in_len), 4096);
        if (n <= 0) break;
        in_len += n;
    }
    syscall3(17, 1, 0, 0);
    return 0;
}

static int write_output(const char *path) {
    syscall3(19, (int)path, 0, 0);
    if (syscall3(13, (int)path, 0, 0) < 0) return -1;
    if (syscall3(14, 2, (int)path, 1) != 0) return -1;
    int w = syscall3(15, 2, (int)code, code_len);
    syscall3(17, 2, 0, 0);
    return w == code_len ? 0 : -1;
}

/* ================= 入口 =================
 * 与 cc500 同款签名：ccrun/micc 经 exec 传入 (argv 指针, argc)；
 * 本文件为完整 C，直接以 char** 读取 argv[1]=输入 argv[2]=输出。 */

int minicc_main(char *argv, int argc) {
    char **av = (char **)argv;
    const char *in_path = "/minicc.c";
    const char *out_path = "/out.elf";
    if (argc >= 2 && av[1]) in_path = av[1];
    if (argc >= 3 && av[2]) out_path = av[2];

    if (open_input(in_path) != 0) {
        sys_print("minicc: input open fail\n");
        return 1;
    }

    /* ---- 编译：词法/语法建树 ---- */
    code = (unsigned char *)xmalloc(8192);
    code_cap = 8192;
    code_len = 0;
    nsym = 0; npatch = 0; nlab = 0;
    funcs = NULL; funcs_tail = &funcs;
    gvars = NULL; gvars_tail = &gvars;
    src = in_data;
    src_len = in_len;
    src_pos = 0;
    next_tok();
    parse_program();
    if (tok[0] != 0) fail("unexpected token");

    /* ---- 代码生成：ELF 头 + 入口 stub + 全局数据 + 函数 ---- */
    {
        static const unsigned char hdr[95] = {
            0x7f,0x45,0x4c,0x46,0x01,0x01,0x01,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x02,0x00,0x03,0x00,0x01,0x00,0x00,0x00, 0x54,0x00,0x0a,0x80,0x34,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x34,0x00,0x20,0x00,0x01,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x0a,0x80,
            0x00,0x00,0x0a,0x80,0x10,0x4b,0x00,0x00, 0x10,0x4b,0x00,0x00,0x07,0x00,0x00,0x00,
            0x00,0x10,0x00,0x00,
            0xe8,0x00,0x00,0x00,0x00, 0x89,0xc3,0x31,0xc0,0xcd,0x80
        };
        for (int i = 0; i < 95; i++) emit1(hdr[i]);
    }
    patch_add("main", 0x55, P_CALL);
    for (Node *g = gvars; g; g = g->next) gen_global(g);
    for (Node *f = funcs; f; f = f->next) gen_func(f);
    finish();

    if (write_output(out_path) != 0) {
        sys_print("minicc: output write fail\n");
        return 1;
    }
    sys_print("minicc: compiled OK\n");
    return 0;
}
