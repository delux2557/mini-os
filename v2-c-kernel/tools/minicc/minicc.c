/* mini-os/v2-c-kernel/tools/minicc/minicc.c
 * minicc —— mini-os 自研小型 C 编译器（V1：int-only）。
 *
 * 版权与许可：
 *   Copyright (C) 2026 mini-os authors
 *   SPDX-License-Identifier: MIT
 *   本项目（mini-os）整体为 MIT 许可。cc500 为 GPL 工具链组件，minicc 是
 *   全新的独立实现（MIT），用于逐步取代 cc500 作为 mini-os 的板载编译器。
 *
 * V1 范围（刻意从简，见 docs/history/external-reviews/mini-os-cc500-deep-audit.md）：
 *   类型：  仅 int（32 位有符号）。无 char / 指针 / 数组 / 结构体 / 字符串。
 *   语句：  { 块 }、int 局部声明（可带初始化）、if/else、while、return、表达式语句。
 *   表达式：整型字面量（十进制）、标识符（局部/参数/全局）、()、一元 - 与 !、
 *          二元 * / % + - < <= > >= == != && ||，赋值 =（右结合，须左值）。
 *   函数：  任意个 int 参数、int 返回值、递归；支持全局 int 变量（可选常量初始化）。
 *   注释：  块注释（斜杠星号...星号斜杠）与 // 行注释。
 *   不支持：预处理、指针、数组下标、字符串、结构体、for/do/switch/break/continue。
 *
 * 代码生成（x86 32 位，直接产出 ELF32，无汇编/链接环节）：
 *   - 标准 ebp 栈帧；表达式用"栈式"求值：每步结果在 eax，二元运算 push 左操作数。
 *   - 调用约定（自洽，编译器的调用方与生成方一致）：
 *       参数从左到右求值并逐个 push；被调函数第 i 个形参位于 [ebp+8+4*(n-1-i)]。
 *       返回值在 eax；eax/ebx/ecx/edx 为调用者保存，其余寄存器被调用者保存。
 *   - ELF 布局：0x00 ELF 头 + 程序头（p_filesz/memsz 收尾回填），0x54 入口 stub：
 *       call main ; mov %eax,%ebx ; xor %eax,%eax ; int $0x80
 *     即以 main() 返回值作 sys_exit 退出码（mini-os ABI：eax=号 ebx=a 返回 ebx）。
 *
 * 运行环境：
 *   - guest：由 minicc_crt.c 提供 syscall3（int $0x80）与 _start；
 *   - host： 由 tools/minicc/host_crt.c 提供 syscall3（Linux 文件模拟）。
 *   本文件只依赖 syscall3(14/16/13/15/17/19/35/1/0)（FS 槽位读写 + brk + print + exit），
 *   不依赖 libc，同一份源码可在宿主（gcc 直编）与 guest（-ffreestanding）双端构建。
 *
 * 自举说明：V1 编译器本体用完整宿主 C 编写（便于代码清晰），尚未限制到自身子集；
 *   V2 引入指针/字符串后，将把本源码逐步收紧为 minicc 子集并做 P1==P2 自举验证。
 */

#include <stdint.h>

/* ================= 系统调用与运行时基础 ================= */

int syscall3(int n, int a, int b, int c);   /* 由 CRT 提供 */

/* 表达式类别：T_LVAL = eax 持左值地址；T_RVAL = eax 持值 */
enum { T_LVAL = 1, T_RVAL = 2 };

static void sys_print(const char *s) {
    syscall3(1, (int)s, 0, 0);              /* SYS_PRINT */
}

/* 编译错误：打印上下文 token 后以 1 退出（host/guest 均由 sys_exit 兜底） */
const char *tokbuf_current(void);       /* 前向声明：定义在词法章节 */
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

/* brk 分配（guest 走 SYS_BRK 移动 program break；host shim 用静态竞技场模拟） */
static void *xmalloc(int n) {
    uint32_t old = (uint32_t)syscall3(35, 0, 0, 0);          /* SYS_BRK 查询 */
    if (syscall3(35, (int)(old + (uint32_t)n), 0, 0) != 0) { /* SYS_BRK 上移 */
        sys_print("minicc: out of memory\n");
        syscall3(0, 1, 0, 0);
    }
    return (void *)(uint32_t)old;
}

/* ================= 输出码流缓冲 ================= */

static unsigned char *code;         /* 编译产物（ELF 头 + 机器码 + 全局数据） */
static int code_len;                /* 已写字节数 */
static int code_cap;                /* 缓冲容量 */

static void emit1(int b) {
    if (code_len >= code_cap) {
        unsigned char *n = (unsigned char *)xmalloc(code_cap * 2);
        for (int i = 0; i < code_len; i++) n[i] = code[i];
        code = n;
        code_cap *= 2;
    }
    code[code_len++] = (unsigned char)b;
}

static void emit4(int v) {          /* 小端 32 位 */
    emit1(v & 0xff); emit1((v >> 8) & 0xff);
    emit1((v >> 16) & 0xff); emit1((v >> 24) & 0xff);
}

static void emit_op(const char *s) {
    while (*s) emit1((unsigned char)*s++);
}

static void save32(int pos, int v) { /* 回填 4 字节（补丁用） */
    code[pos]     = (unsigned char)(v & 0xff);
    code[pos + 1] = (unsigned char)((v >> 8) & 0xff);
    code[pos + 2] = (unsigned char)((v >> 16) & 0xff);
    code[pos + 3] = (unsigned char)((v >> 24) & 0xff);
}

/* ================= 符号表 ================= */

#define SYM_MAX 512
enum { K_FUNC, K_GLOBAL, K_LOCAL, K_ARG };

typedef struct {
    char name[32];
    int kind;                       /* K_* */
    int val;                        /* FUNC: 代码偏移(未定义=-1)；GLOBAL: 数据偏移；
                                       LOCAL: 局部序号；ARG: 参数序号 */
} Sym;

static Sym syms[SYM_MAX];
static int nsym;

static int sym_find(const char *name) {
    for (int i = 0; i < nsym; i++)
        if (s_eq(syms[i].name, name)) return i;
    return -1;
}

static int sym_add(const char *name, int kind, int val) {
    if (nsym >= SYM_MAX) fail("symbol table full");
    s_cpy(syms[nsym].name, name);
    syms[nsym].kind = kind;
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
enum { L_COND, L_JMP };             /* 0F 8x rel32（6B）/ E9 rel32（5B） */
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

/* ================= 代码生成辅助 ================= */

#define CODE_BASE 0x800A0000u       /* APP_LINK：与内核 ELF 加载器一致 */

static int cur_nargs;               /* 当前函数形参数 */
static int cur_nlocals;             /* 当前函数局部变量数 */
static int frame_patch;             /* prologue `sub esp,imm32` 的 imm 位置 */

/* eax = [ebp + disp]（disp32 形式，无 127 字节上限） */
static void emit_lea_ebp(int disp) {
    emit_op("\x8d\x85"); emit4(disp);
}

/* eax = 左值地址（标识符）或常量（数字），并推进 token */
static void emit_mov_imm(int v) {
    emit1(0xB8); emit4(v);
}

/* 若 eax 是左值地址则加载为值（注意不能走 emit_op 字符串：含 \x00 会截断） */
static void promote(int t) {
    if (t == T_LVAL) { emit1(0x8B); emit1(0x00); }   /* mov (%eax),%eax */
}

/* add esp, n4（清除调用参数） */
static void emit_add_esp(int n4) {
    if (n4 <= 127) { emit_op("\x83\xc4"); emit1(n4); }
    else           { emit_op("\x81\xc4"); emit4(n4); }
}

/* ================= 词法 ================= */

#define TOK_MAX 64
static const unsigned char *src;    /* 输入源码（整读入内存） */
static int src_len;
static int src_pos;
static char tok[TOK_MAX];           /* 当前 token */
static int tok_is_word;             /* 标识符 / 关键字 */
static int tok_is_num;              /* 十进制数字 */

static int peekc(void)      { return src_pos < src_len ? src[src_pos] : -1; }
static int peekc2(void)     { return src_pos + 1 < src_len ? src[src_pos + 1] : -1; }

const char *tokbuf_current(void) { return tok; }

static void next_tok(void) {
    /* 跳过空白与注释 */
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
    /* 标识符 / 关键字 */
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
    /* 数字：仅十进制，禁止字母混入（如 123abc / 0x10） */
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
    /* 运算符：先试双字符，再单字符 */
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
    tok[0] = (char)c; tok[1] = 0;   /* 其它字符：由 parser 报"expected"错误 */
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

/* ================= 表达式（递归下降，返回 T_LVAL / T_RVAL） ================= */

static int expr(void);

static int primary(void) {
    if (tok_is_num) {
        int v = 0;
        for (int i = 0; tok[i]; i++) {
            if (tok[i] < '0' || tok[i] > '9') fail("bad number");
            v = v * 10 + (tok[i] - '0');
        }
        emit_mov_imm(v);
        next_tok();
        return T_RVAL;
    }
    if (tok_is_word) {
        char name[32];
        s_cpy(name, tok);
        next_tok();
        /* 函数调用：后随 '(' 且符号为函数（或尚未声明的隐式函数） */
        if (peek("(")) {
            int si = sym_find(name);
            if (si >= 0 && syms[si].kind != K_FUNC) fail("call to non-function");
            if (si < 0) sym_add(name, K_FUNC, -1);   /* 隐式声明，收尾校验定义 */
            expect("(");
            int nargs = 0;
            while (!peek(")")) {
                promote(expr());
                emit1(0x50);                        /* push %eax（从左到右） */
                nargs++;
                if (!accept(",")) break;
            }
            expect(")");
            emit1(0xE8); emit4(0);                  /* call rel32（占位） */
            patch_add(name, code_len - 4, P_CALL);
            emit_add_esp(nargs * 4);
            return T_RVAL;
        }
        int si = sym_find(name);
        if (si < 0) fail("undefined variable");
        if (syms[si].kind == K_LOCAL)
            emit_lea_ebp(-4 * (syms[si].val + 1));
        else if (syms[si].kind == K_ARG)
            emit_lea_ebp(8 + 4 * (cur_nargs - 1 - syms[si].val));
        else if (syms[si].kind == K_GLOBAL) {
            emit_mov_imm(0);
            patch_add(name, code_len - 4, P_ADDR);
        }
        else fail("not a variable");
        return T_LVAL;
    }
    if (accept("(")) {
        int t = expr();
        expect(")");
        return t;
    }
    fail("bad expression");
    return T_RVAL;      /* 不可达 */
}

static int unary(void) {
    if (accept("-")) { promote(unary()); emit_op("\xf7\xd8"); return T_RVAL; }  /* neg %eax */
    if (accept("!")) {
        promote(unary());
        emit_op("\x85\xc0\x0f\x94\xc0\x0f\xb6\xc0");   /* test; sete; movzbl */
        return T_RVAL;
    }
    return primary();
}

/* 二元运算通用序列（宏保证生成顺序：先 push 左值，再求右值，最后运算）。
 * 注意不能写成函数+参数：C 会先求右操作数参数，覆盖 eax 后再 push 左值。 */
#define BINOP(rhs, ops) do { \
    promote(t); \
    emit1(0x50);            /* push 左值 */ \
    { int rt = (rhs); promote(rt); } \
    emit_op(ops);           /* pop %ebx 后运算，结果回 eax */ \
    t = T_RVAL; \
} while (0)

static int mul(void) {
    int t = unary();
    for (;;) {
        if (accept("*"))      BINOP(unary(), "\x5b\x0f\xaf\xc3");          /* imul %ebx,%eax */
        else if (accept("/")) BINOP(unary(), "\x5b\x87\xd8\x99\xf7\xfb");  /* xchg; cdq; idiv %ebx */
        else if (accept("%")) BINOP(unary(), "\x5b\x87\xd8\x99\xf7\xfb\x89\xd0");
        else return t;
    }
}

static int add(void) {
    int t = mul();
    for (;;) {
        if (accept("+"))      BINOP(mul(), "\x5b\x01\xd8");                 /* add %ebx,%eax */
        else if (accept("-")) BINOP(mul(), "\x5b\x29\xc3\x89\xd8");         /* sub; mov */
        else return t;
    }
}

static int rel(void) {
    int t = add();
    for (;;) {
        if (accept("<"))       BINOP(add(), "\x5b\x39\xc3\x0f\x9c\xc0\x0f\xb6\xc0");
        else if (accept("<=")) BINOP(add(), "\x5b\x39\xc3\x0f\x9e\xc0\x0f\xb6\xc0");
        else if (accept(">"))  BINOP(add(), "\x5b\x39\xc3\x0f\x9f\xc0\x0f\xb6\xc0");
        else if (accept(">=")) BINOP(add(), "\x5b\x39\xc3\x0f\x9d\xc0\x0f\xb6\xc0");
        else return t;
    }
}

static int eq(void) {
    int t = rel();
    for (;;) {
        if (accept("==")) BINOP(rel(), "\x5b\x39\xc3\x0f\x94\xc0\x0f\xb6\xc0");
        else if (accept("!=")) BINOP(rel(), "\x5b\x39\xc3\x0f\x95\xc0\x0f\xb6\xc0");
        else return t;
    }
}

static int land(void) {
    int t = eq();
    while (accept("&&")) {
        /* 注意：两次 jz 必须用独立标签（补丁表每条记录只回填一处），再指向同一目标 */
        int fa1 = new_lab(), fa2 = new_lab(), en = new_lab();
        promote(t); emit_op("\x85\xc0"); emit_cond(0x84, fa1);   /* jz fa（左操作数假） */
        t = eq();
        promote(t); emit_op("\x85\xc0"); emit_cond(0x84, fa2);   /* jz fa（右操作数假） */
        emit_mov_imm(1);
        emit_jmp(en);
        patch_lab(fa1, code_len);
        patch_lab(fa2, code_len);
        emit_op("\x31\xc0");                                    /* xor %eax,%eax */
        patch_lab(en, code_len);
        t = T_RVAL;
    }
    return t;
}

static int lor(void) {
    int t = land();
    while (accept("||")) {
        int tr1 = new_lab(), tr2 = new_lab(), en = new_lab();
        promote(t); emit_op("\x85\xc0"); emit_cond(0x85, tr1);   /* jne tr（左操作数真） */
        t = land();
        promote(t); emit_op("\x85\xc0"); emit_cond(0x85, tr2);   /* jne tr（右操作数真） */
        emit_op("\x31\xc0");
        emit_jmp(en);
        patch_lab(tr1, code_len);
        patch_lab(tr2, code_len);
        emit_mov_imm(1);
        patch_lab(en, code_len);
        t = T_RVAL;
    }
    return t;
}

static int expr(void) {
    int t = lor();
    if (accept("=")) {
        if (t != T_LVAL) fail("assign to non-lvalue");
        emit1(0x50);                            /* push 左值地址 */
        promote(expr());                        /* 右值 → eax */
        emit_op("\x5b\x89\x03");                /* pop %ebx; mov %eax,(%ebx) */
        return T_RVAL;
    }
    return t;
}

/* ================= 语句 ================= */

static void statement(void) {
    if (accept("{")) {
        int mark = nsym;
        while (!peek("}")) {
            if (tok[0] == 0) fail("unexpected end of file");
            statement();
        }
        expect("}");
        nsym = mark;
        return;
    }
    if (accept("int")) {
        if (!tok_is_word) fail("expected identifier");
        char name[32];
        s_cpy(name, tok);
        next_tok();
        if (cur_nlocals >= 512) fail("too many locals");
        int v = cur_nlocals++;
        sym_add(name, K_LOCAL, v);
        if (accept("=")) {
            promote(expr());
            emit1(0x50);                        /* push 初值 */
            emit_lea_ebp(-4 * (v + 1));         /* 左值地址 → eax */
            emit_op("\x5b\x89\x03");            /* pop %ebx; mov %eax,(%ebx) */
        }
        expect(";");
        return;
    }
    if (accept("if")) {
        expect("(");
        promote(expr());
        expect(")");
        int els = new_lab(), en = new_lab();
        emit_op("\x85\xc0"); emit_cond(0x84, els);      /* test; jz else */
        statement();
        emit_jmp(en);
        patch_lab(els, code_len);
        if (accept("else")) statement();
        patch_lab(en, code_len);
        return;
    }
    if (accept("while")) {
        expect("(");
        int top = code_len;
        promote(expr());
        expect(")");
        int en = new_lab();
        emit_op("\x85\xc0"); emit_cond(0x84, en);       /* test; jz end */
        statement();
        emit_jmp_to(top);
        patch_lab(en, code_len);
        return;
    }
    if (accept("return")) {
        if (!peek(";")) promote(expr());
        expect(";");
        emit_op("\x89\xec\x5d\xc3");                    /* mov %esp,%ebp; pop %ebp; ret */
        return;
    }
    promote(expr());
    expect(";");
}

/* ================= 程序（全局声明 + 函数定义） ================= */

static void program(void) {
    for (;;) {
        if (tok[0] == 0) return;
        if (!accept("int")) fail("expected 'int'");
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
                si = sym_add(name, K_FUNC, -1);
            }
            int func_scope = nsym;              /* 形参/局部变量从全局符号分界 */
            cur_nargs = 0; cur_nlocals = 0;
            if (!peek(")")) {
                for (;;) {
                    if (!accept("int")) fail("expected 'int' in parameters");
                    if (!tok_is_word) fail("expected parameter name");
                    char pn[32];
                    s_cpy(pn, tok);
                    next_tok();
                    sym_add(pn, K_ARG, cur_nargs);
                    cur_nargs++;
                    if (!accept(",")) break;
                }
            }
            expect(")");
            syms[si].val = code_len;            /* 函数入口 */
            emit_op("\x55\x89\xe5");            /* push %ebp; mov %esp,%ebp */
            emit_op("\x81\xec"); emit4(0);      /* sub $0,%esp（帧大小收尾回填） */
            frame_patch = code_len - 4;
            if (!accept("{")) fail("expected function body");
            while (!peek("}")) {
                if (tok[0] == 0) fail("unexpected end of file");
                statement();
            }
            expect("}");
            emit_op("\x89\xec\x5d\xc3");        /* mov %esp,%ebp; pop %ebp; ret */
            save32(frame_patch, cur_nlocals * 4);
            nsym = func_scope;                  /* 丢弃形参/局部符号 */
        } else {
            /* ---- 全局变量 ---- */
            if (sym_find(name) >= 0) fail("redefined");
            int pos = code_len;
            emit4(0);                           /* 数据占位（4 字节） */
            sym_add(name, K_GLOBAL, pos);
            if (accept("=")) {                  /* V1：仅整型字面量初始化 */
                if (!tok_is_num) fail("global init must be a constant");
                int v = 0;
                for (int i = 0; tok[i]; i++) v = v * 10 + (tok[i] - '0');
                save32(pos, v);
                next_tok();
            }
            expect(";");
        }
    }
}

/* ================= 收尾：回填补丁、校验、写文件 ================= */

static void finish(void) {
    /* 1) 控制流标签回填 */
    for (int i = 0; i < nlab; i++) {
        int imm = labs[i].pos + (labs[i].kind == L_COND ? 2 : 1);
        int rel = labs[i].target - (labs[i].pos + (labs[i].kind == L_COND ? 6 : 5));
        save32(imm, rel);
    }
    /* 2) 符号引用回填（未定义函数在此时报错） */
    for (int i = 0; i < npatch; i++) {
        int si = sym_find(patches[i].name);
        if (patches[i].kind == P_CALL) {
            if (si < 0 || syms[si].kind != K_FUNC || syms[si].val < 0)
                fail("undefined function");
            /* rel32 用相对坐标：目标(函数偏移) - 下一条(call 后 4 字节)，CODE_BASE 抵消 */
            save32(patches[i].pos, syms[si].val - (patches[i].pos + 4));
        } else {
            if (si < 0 || syms[si].kind != K_GLOBAL) fail("internal: bad addr patch");
            save32(patches[i].pos, (int)(CODE_BASE + (uint32_t)syms[si].val));
        }
    }
    /* 3) ELF 程序头 p_filesz / p_memsz 回填 */
    save32(68, code_len);
    save32(72, code_len);
}

/* ================= mini-os 文件系统 I/O（FS 槽位读写） =================
 * SYS_PRINT=1 SYS_FS_CREATE=13 SYS_FS_OPEN=14 SYS_FS_WRITE=15 SYS_FS_READ=16
 * SYS_FS_CLOSE=17 SYS_FS_DELETE=19 SYS_BRK=35 SYS_EXIT=0 */

static unsigned char *in_data;
static int in_len;

static int open_input(const char *path) {
    if (syscall3(14, 1, (int)path, 0) != 0) return -1;      /* slot1 只读 */
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
    syscall3(19, (int)path, 0, 0);                          /* 删旧文件 */
    if (syscall3(13, (int)path, 0, 0) < 0) return -1;       /* 新建 */
    if (syscall3(14, 2, (int)path, 1) != 0) return -1;      /* slot2 只写 */
    int w = syscall3(15, 2, (int)code, code_len);
    syscall3(17, 2, 0, 0);
    return w == code_len ? 0 : -1;
}

/* ================= 入口 =================
 * 与 cc500 同款签名：ccrun 经 exec 传入 (argv 指针, argc)；
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

    /* ---- 编译 ---- */
    code = (unsigned char *)xmalloc(8192);
    code_cap = 8192;
    code_len = 0;
    nsym = 0; npatch = 0; nlab = 0;
    {
        /* ELF32 头 + 程序头（link 0x800A0000）+ 入口 stub：
         *   call main ; mov %eax,%ebx ; xor %eax,%eax ; int $0x80
         * call 的 rel32 在 0x55，作为对 "main" 的补丁 */
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

    src = in_data;
    src_len = in_len;
    src_pos = 0;
    next_tok();
    program();
    if (tok[0] != 0) fail("unexpected token");
    finish();

    if (write_output(out_path) != 0) {
        sys_print("minicc: output write fail\n");
        return 1;
    }
    sys_print("minicc: compiled OK\n");
    return 0;
}
