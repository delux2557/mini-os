/* mini-os/v2-c-kernel/tools/minicc/minicc.c
 * minicc —— mini-os 自研小型 C 编译器（V2d：数组 int a[N] 局部/全局 + a[i] 读写）。
 *
 * 版权与许可：
 *   Copyright (C) 2026 mini-os authors
 *   SPDX-License-Identifier: MIT
 *   本项目（mini-os）整体为 MIT 许可。cc500 为 GPL 工具链组件，minicc 是
 *   全新的独立实现（MIT），用于逐步取代 cc500 作为 mini-os 的板载编译器。
 *
 * 架构（详见 docs/design/minicc-design.md）：
 *   V1 为单遍直接生成（无 AST）；V2a 起引入显式 AST：Parser 建树、Codegen 遍历；
 *   V2b 引入类型系统与指针（TY_INT/TY_PTR）；V2c 引入 TY_CHAR 与字符串字面量、
 *     并为隐式声明的 syscall3 生成 int $0x80 包装（产物可观察 I/O）；
 *   V2d 引入 TY_ARRAY：局部/全局定长数组（int/char 元素），a[i] 读写，&a[i]
 *     可作指针；帧布局由"4 字节槽"改为"字节偏移"（数组按元素尺寸紧凑分配）。
 *
 * 当前子集（V2d）：
 *   类型 int / char / int* / char*（无多级指针）/ int[N] / char[N]（无数组初始化）；
 *   字面量十进制 + 字符串 + 字符；字符串字面量（\n \t \\ \" \' \0 \xNN 转义，
 *   ≤254 字节，入只读数据段，隐式 char*）；局部/参数/全局变量（全局仅常量初始化）；
 *   + - * / % < <= > >= == != && || ! 一元负号、赋值；& 取地址、* 解引用、a[i] 下标
 *   （下标边界不检查，UB 由用户负责）；{} if/else while for return 表达式语句；
 *   多参数函数、递归、前向调用；块注释与 // 行注释；隐式声明并调用
 *   syscall3(n,a,b,c)（编译器生成机器码 stub）。
 *
 * 代码生成（x86 32 位，直出 ELF32）：
 *   - 标准 ebp 栈帧；表达式值在 eax；二元运算 push 左操作数。
 *   - 调用约定（自洽）：参数从左到右求值逐个 push；第 i 个形参位于 [ebp+8+4*(n-1-i)]。
 *   - ELF 布局：0x00 头+程序头，0x54 入口 stub
 *       call main ; mov %eax,%ebx ; xor %eax,%eax ; int $0x80
 *     （main 返回值作 sys_exit 退出码：mini-os ABI eax=号 ebx=a 返回 ebx）。
 *   - char 值无符号（movzbl 读 / mov %al 写）；指针算术按基类型缩放（int×4，char×1）；
 *     局部帧按字节偏移分配（数组紧凑，char 标量 4 字节对齐）。
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
static int src_pos;                     /* fail 调试定位用（词法区定义，前向声明） */
static int src_len;
static const unsigned char *src;

/* 编译错误：打印上下文 token 后以 1 退出（host/guest 均由 sys_exit 兜底） */
static void fail(const char *msg) {
    sys_print("minicc: error: ");
    sys_print(msg);
    sys_print(" [");
    sys_print(tokbuf_current());
    sys_print("] @");
    {   /* 调试：打印 src_pos（十进制）与上下文源码行 */
        int p = src_pos, d = 1000000, f = 0;
        char buf[16]; int bi = 0;
        if (p == 0) { buf[bi++] = '0'; }
        while (d > 0) {
            int q = p / d; p = p % d;
            if (q || f) { buf[bi++] = (char)('0' + q); f = 1; }
            d = d / 10;
        }
        buf[bi] = 0;
        sys_print(buf);
    }
    sys_print(" pos ");
    {   /* 调试：打印错误 token 前的原始源码上下文 */
        int st = src_pos - 30; if (st < 0) st = 0;
        int en = src_pos + 30; if (en > src_len) en = src_len;
        sys_print(" ctx[");
        for (int i = st; i < en; i++) {
            char bb[2];
            if (src[i] == '\n') { sys_print("\\n"); continue; }
            bb[0] = (char)src[i]; bb[1] = 0;
            sys_print(bb);
        }
        sys_print("]\n");
    }
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
enum { TY_INT, TY_CHAR, TY_PTR, TY_ARRAY };  /* V2c：char（无符号）；V2d：数组 */

typedef struct {
    char name[32];
    int kind;
    int ty;                     /* TY_INT / TY_CHAR / TY_PTR / TY_ARRAY */
    int bty;                    /* 指针基类型 / 数组元素类型（非指针数组时无意义） */
    int len;                    /* V2d：数组元素个数（非数组=0） */
    int val;                    /* FUNC: 代码偏移(未定义=-1)；GLOBAL: 数据偏移；
                                   LOCAL: 帧字节偏移；ARG: 参数序号 */
} Sym;

static Sym syms[SYM_MAX];
static int nsym;

static int sym_find(const char *name) {
    /* V3：从后往前查找（最近声明优先）——局部变量遮蔽同名的全局函数/变量
     * （如 finish() 的局部 rel 遮蔽解析函数 rel，BUG-035）。 */
    for (int i = nsym - 1; i >= 0; i--)
        if (s_eq(syms[i].name, name)) return i;
    return -1;
}

static int sym_add(const char *name, int kind, int ty, int bty, int len, int val) {
    if (nsym >= SYM_MAX) fail("symbol table full");
    s_cpy(syms[nsym].name, name);
    syms[nsym].kind = kind;
    syms[nsym].ty = ty;
    syms[nsym].bty = bty;
    syms[nsym].len = len;
    syms[nsym].val = val;
    return nsym++;
}

/* ================= 补丁（符号引用 / 控制流标签） ================= */

#define PATCH_MAX 4096         /* V3：自举需 ~2K 引用（函数调用+全局地址），1024 溢出 */
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
    ND_NUM, ND_STR, ND_VAR, ND_FUNCALL,   /* V2c：ND_STR 字符串字面量（ival=池偏移） */
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD,
    ND_BITAND, ND_BITOR, ND_BITXOR,       /* V3a：& | ^ */
    ND_SHL, ND_SHR,                       /* V3a：<< >> */
    ND_EQ, ND_NE, ND_LT, ND_LE, ND_GT, ND_GE,
    ND_AND, ND_OR, ND_NEG, ND_NOT, ND_BNOT,  /* V3a：ND_BNOT ~ */
    ND_ASSIGN,
    ND_ADDR, ND_DEREF,         /* V2b：& 取地址 / * 解引用 */
    ND_INDEX,                  /* V2d：a[i] 下标（l=数组 VAR，r=下标表达式，左值） */
    ND_EXPR_STMT, ND_BLOCK, ND_IF, ND_WHILE, ND_FOR, ND_RET,
    ND_DECL, ND_FUNC, ND_GVAR
};

typedef struct Node Node;
struct Node {
    int kind;
    int ty;             /* TY_INT / TY_CHAR / TY_PTR / TY_ARRAY：表达式类型 */
    int bty;            /* V2c：指针基类型/数组元素类型（DEREF/INDEX 结果的类型来源） */
    int len;            /* V2d：数组元素个数（DECL/GVAR 用） */
    Node *l, *r;        /* 二元操作数；ASSIGN: l=左值 r=右值；IF: l=cond r=then；
                           WHILE: l=cond r=body；RET: l=表达式；NOT/NEG/ADDR/DEREF: l=操作数；
                           INDEX: l=数组 VAR r=下标 */
    Node *a, *b;        /* FUNCALL: a=实参链表；FUNC: a=形参链表 b=函数体；
                           IF: b=else 分支（可空）；BLOCK: a=语句链表 */
    Node *next;         /* 语句/实参/形参链表的后继 */
    int val;            /* NUM: 数值；STR: 无；DECL: 帧字节偏移；FUNC/GVAR: 符号下标；VAR: 无 */
    int vkind, vslot;   /* VAR: 变量类别（K_LOCAL/K_ARG/K_GLOBAL）与偏移/序号（local 字节偏移）——
                           节点脱离符号表自携带，作用域恢复丢弃符号后仍可正确生成 */
    char vname[32];     /* VAR: 全局变量名（patch 用） */
    int ival;           /* STR: 字符串池偏移；GVAR: 常量初始化值（无初始化=0） */
    int nargs, nlocals; /* FUNC: 形参个数 / 帧大小（字节）；FUNCALL: 实参个数 */
    char name[32];      /* FUNC/GVAR/FUNCALL: 名字 */
};

static Node *node_new(int kind) {
    Node *n = (Node *)xmalloc(sizeof(Node));
    n->kind = kind;
    n->ty = TY_INT;
    n->bty = 0;
    n->len = 0;
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
static void emit_load8(void) { emit1(0x0F); emit1(0xB6); emit1(0x00); } /* movzbl (%eax),%eax（char 无符号读；勿用 emit_op：\x00 截断） */
static void emit_store(void) { emit_op("\x5b\x89\x03"); }        /* pop %ebx; mov %eax,(%ebx) */
static void emit_store8(void) { emit_op("\x5b\x88\x03"); }       /* pop %ebx; mov %al,(%ebx) */
static void emit_test(void) { emit_op("\x85\xc0"); }
static void emit_epilogue(void) { emit_op("\x89\xec\x5d\xc3"); } /* mov %esp,%ebp; pop %ebp; ret */

static void emit_add_esp(int n4) {
    if (n4 <= 127) { emit_op("\x83\xc4"); emit1(n4); }
    else           { emit_op("\x81\xc4"); emit4(n4); }
}

/* ================= 词法 ================= */

#define TOK_MAX 256            /* V2c：字符串字面量上限 254 字节（含解码） */
static char tok[TOK_MAX];
static int tok_is_word;
static int tok_is_num;
static int tok_is_str;         /* V2c：字符串字面量（tok=解码字节，toklen=长度） */
static int tok_is_char;        /* V2c：字符字面量（tok[0]=解码值 0..255） */
static int toklen;             /* V2c：tok 解码字节数（字符串可含内嵌 \0） */

static int peekc(void)  { return src_pos < src_len ? src[src_pos] : -1; }
static int peekc2(void) { return src_pos + 1 < src_len ? src[src_pos + 1] : -1; }

const char *tokbuf_current(void) { return tok; }

/* 读取 src 处转义序列（src_pos 已越过 '\'），返回解码值；非法转义报错。
 * 借鉴 cc500 已验证的 \xNN/\n/\t 解码，并补充 \\ \" \' \0。 */
static int decode_escape(void) {
    int e = peekc();
    if (e < 0 || e == '\n') fail("bad escape");
    src_pos++;
    if (e == 'n') return 10;
    if (e == 't') return 9;
    if (e == '\\') return '\\';
    if (e == '"') return '"';
    if (e == '\'') return '\'';
    if (e == '0') return 0;
    if (e == 'x') {
        int v = 0, got = 0;
        for (int k = 0; k < 2; k++) {
            int h = peekc();
            if (h >= '0' && h <= '9') v = v * 16 + (h - '0');
            else if (h >= 'a' && h <= 'f') v = v * 16 + (h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v = v * 16 + (h - 'A' + 10);
            else break;
            src_pos++; got = 1;
        }
        if (!got) fail("bad \\x escape");
        return v;
    }
    fail("bad escape");
    return 0;
}

static void next_tok(void) {
    tok_is_word = 0; tok_is_num = 0; tok_is_str = 0; tok_is_char = 0;
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
    if (peekc() < 0) { tok[0] = 0; toklen = 0; return; }

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
        toklen = n;
        tok_is_word = 1;
        return;
    }
    if (c >= '0' && c <= '9') {
        int n = 0;
        tok[n++] = (char)c;
        /* V3a：0x 前缀十六进制字面量（0xNN，支持 a-f/A-F；值须落 int 范围） */
        if (c == '0' && (peekc() == 'x' || peekc() == 'X')) {
            tok[n++] = (char)src[src_pos++];
            while (n < TOK_MAX - 1) {
                int d = peekc();
                if ((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') ||
                    (d >= 'A' && d <= 'F')) { tok[n++] = (char)d; src_pos++; }
                else break;
            }
            tok[n] = 0;
            if (peekc() == '_' || (peekc() >= '0' && peekc() <= '9') ||
                (peekc() >= 'a' && peekc() <= 'z') || (peekc() >= 'A' && peekc() <= 'Z'))
                fail("bad number");
            toklen = n;
            tok_is_num = 1;
            return;
        }
        while (n < TOK_MAX - 1) {
            int d = peekc();
            if (d >= '0' && d <= '9') { tok[n++] = (char)d; src_pos++; }
            else break;
        }
        tok[n] = 0;
        if (peekc() == '_' || (peekc() >= 'a' && peekc() <= 'z') ||
            (peekc() >= 'A' && peekc() <= 'Z'))
            fail("bad number");
        toklen = n;
        tok_is_num = 1;
        return;
    }
    if (c == '"') {                     /* 字符串字面量（V2c）：解码入 tok，EOF/\n 守卫 */
        int n = 0;
        for (;;) {
            int d = peekc();
            if (d < 0 || d == '\n') fail("unterminated string");
            if (d == '"') { src_pos++; break; }
            src_pos++;
            if (d == '\\') d = decode_escape();
            if (n >= TOK_MAX - 2) fail("string too long");   /* 契约：≤254 字节 */
            tok[n++] = (char)d;
        }
        tok[n] = 0;
        toklen = n;
        tok_is_str = 1;
        return;
    }
    if (c == '\'') {                    /* 字符字面量（V2c）：单字节，支持转义 */
        int d = peekc();
        if (d < 0 || d == '\n') fail("unterminated char");
        src_pos++;
        if (d == '\\') d = decode_escape();
        if (peekc() != '\'') fail("char literal too long");
        src_pos++;
        tok[0] = (char)d; tok[1] = 0;
        toklen = 1;
        tok_is_char = 1;
        return;
    }
    int d = peekc();
    if ((c == '=' && d == '=') || (c == '!' && d == '=') ||
        (c == '<' && d == '=') || (c == '>' && d == '=') ||
        (c == '<' && d == '<') || (c == '>' && d == '>') ||
        (c == '&' && d == '&') || (c == '|' && d == '|')) {
        tok[0] = (char)c; tok[1] = (char)d; tok[2] = 0;
        src_pos++;
        toklen = 2;
        return;
    }
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
        c == '<' || c == '>' || c == '=' || c == '!' ||
        c == '&' || c == '|' || c == '^' || c == '~' ||
        c == '(' || c == ')' || c == '{' || c == '}' || c == ';' || c == ',') {
        tok[0] = (char)c; tok[1] = 0;
        toklen = 1;
        return;
    }
    tok[0] = (char)c; tok[1] = 0;
    toklen = 1;
}

static int peek(const char *s) { return s_eq(s, tok); }
/* 符号匹配：字面量 token（word/num/str/char）绝不参与符号比较，
 * 否则字符串 ")" / 字符 ';' 会被误判为括号/分号（BUG-034）。 */
static int is_sym(const char *s) {
    if (tok_is_word || tok_is_num || tok_is_str || tok_is_char) return 0;
    return s_eq(s, tok);
}
static int accept(const char *s) {
    /* 字面量 token（字符/字符串/数字）绝不参与符号匹配：
     * 否则字符字面量 '&' '*' '-' 等会被 unary/二元运算符误消费（BUG-034）。 */
    if (tok_is_char || tok_is_str || tok_is_num) return 0;
    if (peek(s)) { next_tok(); return 1; }
    return 0;
}
static void expect(const char *s) {
    if (!accept(s)) fail("expected token");
}

/* 读取类型声明（int/char 已被调用方 peek，此处消费关键字与 `*`）；
 * 返回 TY_INT / TY_CHAR / TY_PTR，指针基类型经 bty 输出（V2d：不支持多级指针）。
 * 数组后缀 [N] 在标识符之后，由 array_suffix() 单独处理。 */
static int decl_type(int *bty) {
    int base = TY_INT;          /* 初始化避免 -Wmaybe-uninitialized（fail 非 noreturn） */
    if (accept("int")) base = TY_INT;
    else if (accept("char")) base = TY_CHAR;
    else fail("expected type");
    int stars = 0;
    while (accept("*")) stars++;
    if (stars > 1) fail("unsupported: multi-level pointer");
    if (stars) { *bty = base; return TY_PTR; }
    *bty = 0;
    return base;
}

/* V2d：标识符之后的数组后缀 [N]（N 为十进制常量 ≥1，指针数组拒绝）；
 * 命中则置 *len 并返回 1，否则 *len=0 返回 0。 */
static int array_suffix(int ty, int *len) {
    *len = 0;
    if (!accept("[")) return 0;
    if (ty == TY_PTR) fail("unsupported: array of pointers");
    if (!tok_is_num) fail("array size must be a constant");
    int n = 0;
    for (int i = 0; tok[i]; i++) n = n * 10 + (tok[i] - '0');
    if (n < 1) fail("array size must be positive");
    next_tok();
    expect("]");
    *len = n;
    return 1;
}

/* 变量字节大小：int/char/指针标量 4 字节（char 标量对齐）；数组 = len × 元素尺寸 */
static int size_of(int ty, int bty, int len) {
    if (ty == TY_ARRAY) return len * (bty == TY_INT ? 4 : 1);
    return 4;
}

/* 类型等价（契约式语义检查）：指针须基类型一致；int/char 同属整型族可互转；
 * 数组元素（INDEX 结果）按元素类型参与检查。
 * V3（自举）：右值 int 可赋给指针（如 code = xmalloc(n) 的地址值，xmalloc 返回 int）——
 * 这是自举版编译器以 brk 动态分配 code 与输入缓冲的唯一通道（minicc 无 void 指针与类型转换）。 */
static int type_eq(int t1, int b1, int t2, int b2) {
    if (t1 != t2) {
        if (t1 == TY_PTR && t2 == TY_INT) return 1;
        return t1 != TY_PTR && t2 != TY_PTR && t1 != TY_ARRAY && t2 != TY_ARRAY;
    }
    if (t1 != TY_PTR && t1 != TY_ARRAY) return 1;
    return b1 == b2;
}

/* ================= 语法分析（建树） ================= */

static Node *expr(void);

/* 字符串池（V2c）：字面量解码字节线性累积，codegen 时整体 emit 进数据段（只读）。
 * strpool_base：池在 code 缓冲内的起始偏移（ELF 头 95 字节之后），ND_STR 寻址用。 */
static unsigned char *strpool;
static int nstrpool, strpool_cap;
static int strpool_base;

static void strpool_add(int b) {
    if (nstrpool >= strpool_cap) {
        int nc = strpool_cap ? strpool_cap * 2 : 1024;
        unsigned char *np = (unsigned char *)xmalloc(nc);
        for (int i = 0; i < nstrpool; i++) np[i] = strpool[i];
        strpool = np; strpool_cap = nc;
    }
    strpool[nstrpool++] = (unsigned char)b;
}

static Node *primary(void) {
    Node *n;
    if (tok_is_num) {
        n = node_new(ND_NUM);
        n->val = 0;
        if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
            for (int i = 2; tok[i]; i++) {
                int d = tok[i], v = 0;
                if (d >= '0' && d <= '9') v = d - '0';
                else if (d >= 'a' && d <= 'f') v = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') v = d - 'A' + 10;
                else fail("bad number");
                n->val = n->val * 16 + v;
            }
        } else {
            for (int i = 0; tok[i]; i++) {
                if (tok[i] < '0' || tok[i] > '9') fail("bad number");
                n->val = n->val * 10 + (tok[i] - '0');
            }
        }
        next_tok();
        return n;
    }
    if (tok_is_char) {                  /* 字符字面量：无符号 0..255，值为 int */
        n = node_new(ND_NUM);
        n->val = (unsigned char)tok[0];
        next_tok();
        return n;
    }
    if (tok_is_str) {                   /* 字符串字面量：入池，表达式为 char* */
        n = node_new(ND_STR);
        n->ty = TY_PTR;
        n->bty = TY_CHAR;
        n->ival = nstrpool;
        for (int i = 0; i < toklen; i++) strpool_add((unsigned char)tok[i]);
        strpool_add(0);                 /* NUL 终止 */
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
            if (si < 0) sym_add(name, K_FUNC, TY_INT, 0, 0, -1);   /* 隐式声明 */
            n->val = si < 0 ? nsym - 1 : si;         /* 符号下标 */
            expect("(");
            Node *head = NULL, **tail = &head;
            while (!is_sym(")")) {
                Node *arg = expr();
                *tail = arg; tail = &arg->next;
                n->nargs++;
                if (is_sym(",")) next_tok(); else break;
            }
            expect(")");
            n->a = head;
            return n;
        }
        int si = sym_find(name);
        if (si < 0) fail("undefined variable");
        if (syms[si].ty == TY_ARRAY) {
            /* V2d：数组名必须带下标 a[i]（数组名退化/传参留待后续切片，明确拒绝） */
            n = node_new(ND_INDEX);
            n->l = node_new(ND_VAR);
            n->l->ty = TY_ARRAY;
            n->l->bty = syms[si].bty;
            n->l->vkind = syms[si].kind;
            n->l->vslot = syms[si].val;
            if (n->l->vkind == K_GLOBAL) s_cpy(n->l->vname, syms[si].name);
            if (!accept("[")) fail("array without subscript");
            n->r = expr();
            if (n->r->ty == TY_PTR || n->r->ty == TY_ARRAY) fail("array index must be integer");
            expect("]");
            n->ty = syms[si].bty;               /* 元素类型 */
            n->bty = 0;
            return n;
        }
        n = node_new(ND_VAR);
        n->ty = syms[si].ty;
        n->bty = syms[si].bty;
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
    if (accept("~")) {                  /* V3a：按位取反 */
        Node *n = node_new(ND_BNOT);
        n->l = unary();
        return n;
    }
    if (accept("&")) {
        Node *n = node_new(ND_ADDR);
        n->l = unary();
        /* & 操作数必须可寻址（变量/解引用/数组元素）；类型为指向其的指针（bty=操作数类型） */
        if (n->l->kind != ND_VAR && n->l->kind != ND_DEREF && n->l->kind != ND_INDEX)
            fail("cannot take address");
        n->ty = TY_PTR;
        n->bty = n->l->ty;
        return n;
    }
    if (accept("*")) {
        Node *n = node_new(ND_DEREF);
        n->l = unary();
        if (n->l->ty != TY_PTR) fail("dereference of non-pointer");
        n->ty = n->l->bty;              /* 解引用结果类型 = 指针基类型 */
        return n;
    }
    return primary();
}

static Node *bin(Node *l, Node *r, int kind) {
    Node *n = node_new(kind);
    n->l = l;
    n->r = r;
    /* 指针算术只对 +/- 传播指针类型（bty 随指针侧）；其余运算结果为 int */
    if ((kind == ND_ADD || kind == ND_SUB) &&
        (l->ty == TY_PTR || r->ty == TY_PTR)) {
        n->ty = TY_PTR;
        n->bty = l->ty == TY_PTR ? l->bty : r->bty;
    }
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

static Node *shift(void) {              /* V3a：<< >>（优先级：加之下、关系之上） */
    Node *n = add();
    for (;;) {
        if (accept("<<"))      n = bin(n, add(), ND_SHL);
        else if (accept(">>")) n = bin(n, add(), ND_SHR);
        else return n;
    }
}

static Node *rel(void) {
    Node *n = shift();
    for (;;) {
        if (accept("<"))       n = bin(n, shift(), ND_LT);
        else if (accept("<=")) n = bin(n, shift(), ND_LE);
        else if (accept(">"))  n = bin(n, shift(), ND_GT);
        else if (accept(">=")) n = bin(n, shift(), ND_GE);
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

static Node *bitand(void) {             /* V3a：&（优先级：相等之下、异或之上） */
    Node *n = eq();
    while (accept("&")) n = bin(n, eq(), ND_BITAND);
    return n;
}

static Node *bitxor(void) {
    Node *n = bitand();
    while (accept("^")) n = bin(n, bitand(), ND_BITXOR);
    return n;
}

static Node *bitor(void) {
    Node *n = bitxor();
    while (accept("|")) n = bin(n, bitxor(), ND_BITOR);
    return n;
}

static Node *land(void) {
    Node *n = bitor();
    while (accept("&&")) n = bin(n, bitor(), ND_AND);
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
        if (a->l->kind != ND_VAR && a->l->kind != ND_DEREF && a->l->kind != ND_INDEX)
            fail("assign to non-lvalue");
        if (!type_eq(a->l->ty, a->l->bty, a->r->ty, a->r->bty))
            fail("type mismatch in assignment");
        return a;
    }
    return n;
}

/* ---- 语句 ---- */

static int cur_frame;           /* 当前函数已用帧字节（局部变量偏移分配，块间不回收） */
static int bty_top;             /* 顶层声明（函数/全局）的指针基类型（非指针时无意义） */
static int len_top;             /* 顶层声明（函数/全局）的数组元素个数（非数组=0） */

static Node *stmt(void);

static Node *block_stmt(void) {
    int mark = nsym;
    Node *head = NULL, **tail = &head;
    while (!is_sym("}")) {
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
    if (is_sym("{")) {
        next_tok();
        return block_stmt();
    }
    if (peek("int") || peek("char")) {
        /* 局部声明（`int x;` / `char c;` / `int* p;` / `int a[3];` / `char s[8];`） */
        n = node_new(ND_DECL);
        n->ty = decl_type(&n->bty);
        if (!tok_is_word) fail("expected identifier");
        char name[32];
        s_cpy(name, tok);
        next_tok();
        if (array_suffix(n->ty, &n->len)) { n->bty = n->ty; n->ty = TY_ARRAY; }
        if (n->ty == TY_ARRAY && peek("=")) fail("array init not supported");
        /* V2d：帧按字节偏移分配（数组紧凑 len×元素尺寸，标量 4 字节对齐） */
        int size = size_of(n->ty, n->bty, n->len);
        cur_frame += size;
        if (cur_frame > 4096) fail("frame too big");
        n->val = cur_frame;             /* 帧字节偏移（首个变量 = 4，lea -4(%ebp)） */
        sym_add(name, K_LOCAL, n->ty, n->bty, n->len, n->val);
        if (accept("=")) {
            n->l = expr();
            if (!type_eq(n->ty, n->bty, n->l->ty, n->l->bty))
                fail("type mismatch in initialization");
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
    if (accept("for")) {
        expect("(");
        n = node_new(ND_FOR);
        /* 空初始化 -> NULL；否则解析表达式语句（不消费分号的 expr） */
        if (!is_sym(";")) n->l = expr();
        expect(";");
        /* 条件可空（无限循环） */
        if (!is_sym(";")) n->r = expr();
        expect(";");
        /* 步进可空 */
        if (!is_sym(")")) n->a = expr();
        expect(")");
        n->b = stmt();
        return n;
    }
    if (accept("return")) {
        n = node_new(ND_RET);
        if (!is_sym(";")) n->l = expr();
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
        if (!peek("int") && !peek("char")) fail("expected type");
        int ty = decl_type(&bty_top);   /* 函数返回类型或全局变量类型 */
        if (!tok_is_word) fail("expected identifier");
        char name[32];
        s_cpy(name, tok);
        next_tok();
        len_top = 0;
        if (array_suffix(ty, &len_top)) { bty_top = ty; ty = TY_ARRAY; }
        if (accept("(")) {
            /* ---- 函数定义 ---- */
            int si = sym_find(name);
            if (si >= 0) {
                if (syms[si].kind != K_FUNC || syms[si].val >= 0) fail("redefined");
            } else {
                si = sym_add(name, K_FUNC, TY_INT, 0, 0, -1);
            }
            Node *fn = node_new(ND_FUNC);
            s_cpy(fn->name, name);
            fn->val = si;
            int func_scope = nsym;
            cur_nargs = 0; cur_frame = 0;
            Node *params = NULL, **ptail = &params;
            if (!is_sym(")")) {
                for (;;) {
                    Node *p = node_new(ND_VAR);
                    p->ty = decl_type(&p->bty);
                    if (!tok_is_word) fail("expected parameter name");
                    s_cpy(p->name, tok);
                    next_tok();
                    int plen;
                    if (array_suffix(p->ty, &plen)) fail("unsupported: array parameter");
                    p->vkind = K_ARG;
                    p->vslot = cur_nargs;
                    sym_add(p->name, K_ARG, p->ty, p->bty, 0, cur_nargs);
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
            fn->nlocals = cur_frame;    /* 帧大小（字节） */
            nsym = func_scope;
            *funcs_tail = fn; funcs_tail = &fn->next;
        } else {
            /* ---- 全局变量（数组仅 0 填充；标量常量初始化：数字或字符字面量） ---- */
            if (sym_find(name) >= 0) fail("redefined");
            Node *g = node_new(ND_GVAR);
            s_cpy(g->name, name);
            g->ty = ty;
            g->bty = bty_top;
            g->len = len_top;
            int si = sym_add(name, K_GLOBAL, ty, bty_top, len_top, 0);
            g->val = si;
            if (accept("=")) {
                if (ty == TY_ARRAY) fail("array init not supported");
                if (tok_is_num) {
                    g->ival = 0;
                    /* BUG-039：初值支持十六进制（CODE_BASE=0x800a0000），与 minicc_self.c 的
                     * GVAR 初值环严格一致；旧十进制环把 'x'/'a' 当数字位算成垃圾值。 */
                    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
                        for (int i = 2; tok[i]; i++) {
                            int d = tok[i], v = 0;
                            if (d >= '0' && d <= '9') v = d - '0';
                            else if (d >= 'a' && d <= 'f') v = d - 'a' + 10;
                            else if (d >= 'A' && d <= 'F') v = d - 'A' + 10;
                            else fail("bad hex global init");
                            g->ival = g->ival * 16 + v;
                        }
                    } else {
                        for (int i = 0; tok[i]; i++) g->ival = g->ival * 10 + (tok[i] - '0');
                    }
                    next_tok();
                } else if (tok_is_char) {
                    g->ival = (unsigned char)tok[0];
                    next_tok();
                } else {
                    fail("global init must be a constant");
                }
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
        if (n->vkind == K_LOCAL)      emit_lea_ebp(-n->vslot);   /* V2d：vslot=帧字节偏移 */
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
    if (n->kind == ND_INDEX) {
        /* V2d：a[i] 地址 = 数组基址 + 下标 × 元素尺寸（int×4，char×1 不缩放） */
        gen_addr(n->l);             /* 数组基地址 */
        emit1(0x50);
        gen(n->r);                  /* 下标 */
        if (n->l->bty == TY_INT) emit_op("\xc1\xe0\x02");   /* shl $2,%eax */
        emit_op("\x5b\x01\xd8");    /* pop %ebx; add %ebx,%eax */
        return;
    }
    fail("assign to non-lvalue");
}

static void gen(Node *n) {
    switch (n->kind) {
    case ND_NUM: emit_mov_imm(n->val); return;
    case ND_STR:
        emit_mov_imm((int)(CODE_BASE + (uint32_t)(strpool_base + n->ival))); /* char* -> 数据段 */
        return;
    case ND_VAR:
    case ND_DEREF:
    case ND_INDEX:              /* V2d：a[i] 读（元素按类型取宽） */
        gen_addr(n);
        if (n->ty == TY_CHAR) emit_load8(); else emit_load();
        return;
    case ND_ADDR:
        gen_addr(n->l); return;     /* &x：值 = x 的地址 */
    case ND_NEG: gen(n->l); emit_op("\xf7\xd8"); return;
    case ND_BNOT: gen(n->l); emit_op("\xf7\xd0"); return;      /* V3a：not %eax */
    case ND_NOT:
        gen(n->l); emit_test();
        emit_op("\x0f\x94\xc0\x0f\xb6\xc0");    /* sete al; movzbl al,eax */
        return;
    case ND_ASSIGN:
        gen_addr(n->l); emit1(0x50);
        gen(n->r);
        if (n->l->ty == TY_CHAR) emit_store8(); else emit_store();
        return;
    case ND_BITAND: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x21\xd8"); return;
    case ND_BITOR:  gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x09\xd8"); return;
    case ND_BITXOR: gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x31\xd8"); return;
    case ND_SHL:    gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x87\xd8\x89\xd9\xd3\xe0"); return;
    case ND_SHR:    gen(n->l); emit1(0x50); gen(n->r); emit_op("\x5b\x87\xd8\x89\xd9\xd3\xf8"); return;
    case ND_ADD:
        gen(n->l); emit1(0x50); gen(n->r);
        /* 指针算术：按基类型缩放（int×4，char×1 不缩放） */
        if (n->l->ty == TY_PTR && n->r->ty != TY_PTR) {
            if (n->l->bty == TY_INT) emit_op("\xc1\xe0\x02");       /* shl $2,%eax */
        } else if (n->l->ty != TY_PTR && n->r->ty == TY_PTR) {
            if (n->r->bty == TY_INT) emit_op("\xc1\xe3\x02");       /* shl $2,%ebx */
        }
        emit_op("\x5b\x01\xd8");
        return;
    case ND_SUB:
        gen(n->l); emit1(0x50); gen(n->r);
        if (n->l->ty == TY_PTR && n->r->ty != TY_PTR) {
            if (n->l->bty == TY_INT) emit_op("\xc1\xe0\x02");       /* shl $2,%eax */
        } else if (n->r->ty == TY_PTR) fail("invalid pointer subtraction");
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
            emit_lea_ebp(-n->val);      /* V2d：val=帧字节偏移（数组声明无初始化） */
            emit1(0x50);
            gen(n->l);
            if (n->ty == TY_CHAR) emit_store8(); else emit_store();
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
    case ND_FOR: {
        /* for(init;cond;step)body：l=init, r=cond(可空), a=step(可空), b=body
         * 空 init 时需额外跳过一个空语句（node_new 生 ND_EXPR_STMT(0) 亦可空跑） */
        if (n->l) gen(n->l);
        int top = code_len;
        int en = new_lab();
        if (n->r) { gen(n->r); emit_test(); emit_cond(0x84, en); }
        gen_stmt(n->b);
        if (n->a) gen(n->a);
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

/* 全局数据（ND_GVAR）：emit 变量字节（数组 0 填充；标量回写初值），登记符号偏移 */
static void gen_global(Node *n) {
    int si = n->val;
    int pos = code_len;
    int size = size_of(n->ty, n->bty, n->len);
    for (int i = 0; i < size; i++) emit1(0);
    if (n->ival && n->ty != TY_ARRAY) save32(pos, n->ival);     /* 常量初始化 */
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
    save32(frame_patch, n->nlocals);        /* V2d：nlocals=帧字节数 */
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
    nsym = 0; npatch = 0; nlab = 0; nstrpool = 0;
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
    strpool_base = code_len;
    for (int i = 0; i < nstrpool; i++) emit1(strpool[i]);   /* V2c：字符串池（数据段，只读） */
    for (Node *g = gvars; g; g = g->next) gen_global(g);
    for (Node *f = funcs; f; f = f->next) gen_func(f);
    /* V2c：为隐式声明的 syscall3 生成 int $0x80 包装（用户显式定义则跳过，作普通函数）。
     * 产物因此可声明并调用 syscall3(n,a,b,c) 做可观察 I/O（sys_print/文件）。
     * 注意：隐式声明发生在函数体内、作用域恢复后符号不可见，故以 patch 表扫描触发，
     * 需用时重新登记符号——否则 finish() 会按"undefined function"报错。 */
    for (int i = 0; i < npatch; i++) {
        if (patches[i].kind != P_CALL || !s_eq(patches[i].name, "syscall3")) continue;
        int ss = sym_find("syscall3");
        if (ss < 0) ss = sym_add("syscall3", K_FUNC, TY_INT, 0, 0, -1);
        if (syms[ss].val < 0) {
            syms[ss].val = code_len;
            emit_op("\x55\x89\xe5");            /* push %ebp; mov %esp,%ebp */
            emit_op("\x8b\x45\x14");            /* mov 20(%ebp),%eax  n（minicc 约定：
                                                   参数从左到右 push，第 i 个形参在 [ebp+8+4*(n-1-i)]，
                                                   故第 1 个参数在最深偏移 8+4*(4-1)=20） */
            emit_op("\x8b\x5d\x10");            /* mov 16(%ebp),%ebx  a */
            emit_op("\x8b\x4d\x0c");            /* mov 12(%ebp),%ecx  b */
            emit_op("\x8b\x55\x08");            /* mov 8(%ebp),%edx   c */
            emit1(0xCD); emit1(0x80);           /* int $0x80 */
            emit_op("\x5d\xc3");                /* pop %ebp; ret */
        }
        break;
    }
    finish();

    if (write_output(out_path) != 0) {
        sys_print("minicc: output write fail\n");
        return 1;
    }
    sys_print("minicc: compiled OK\n");
    return 0;
}
