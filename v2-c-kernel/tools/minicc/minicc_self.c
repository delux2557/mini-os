/* mini-os/v2-c-kernel/tools/minicc/minicc_self.c
 * minicc-self —— minicc 的自举版本（V3，编译器本体以自身子集编写）。
 *
 * 版权与许可：Copyright (C) 2026 mini-os authors, SPDX-License-Identifier: MIT
 *
 * 目标（docs/design/minicc-design.md 7.3 / 11）：
 *   用 minicc（V3a，宿主 gcc 构建）编译本文件得 P1；P1 再编译本文件得 P2；
 *   P1 与 P2 逐字节一致 => 自举不动点（ccboot 模式，比差分更强的正确性证明）。
 *
 * 子集纪律（本文件必须完全落于 minicc 支持的子集，任何违规都是 bug）：
 *   - 无 struct/enum/typedef/static/include/宏；无 switch/+=/++/--/?:/多级指针
 *   - 无函数原型声明（minicc 隐式声明 + patch 收集，函数可任意顺序定义）
 *   - 无返回指针的函数（函数返回 int）；AST 用"并行数组 + int 句柄"表达
 *   - 常量用全局 int 变量初始化（数字/字符字面量）；数组大小须为数字字面量
 *   - 数组名作值必须带下标（&arr[0] 取首地址）；指针算术用 *(p+i)
 *   - 动态内存走 syscall3(35) brk：code/输入缓冲为 char*，xmalloc 返回 int 地址
 *     （V3a 放宽"右值 int 可赋给指针"，见 minicc.c type_eq）
 *   - 所有函数返回 int（无 void 类型）
 *   - 入口：main 固定编译 /minicc.c -> /out.elf（与 ccboot 同构）；minicc_main 供
 *     宿主 minicc_crt 链接（cdecl 按名调用，签名不检查）
 */

int sys_print(char* s) { syscall3(1, s, 0, 0); return 0; }

/* ---- 内存池（全部静态并行数组；code/in 经 brk 动态分配） ---- */
int NMAX = 8192;            /* AST 节点池上限 */
int nkind[8192]; int nty[8192]; int nbty[8192]; int nlen[8192];
int nl[8192]; int nr[8192]; int na[8192]; int nb[8192]; int nnext[8192];
int nval[8192]; int nvkind[8192]; int nvslot[8192]; int nival[8192];
int nnargs[8192]; int nnlocals[8192];
int nn;                     /* 当前节点数（0 保留为 NULL） */

char strtab[32768];         /* 名字池（str_add 追加，NUL 终止） */
int nstr;

char strpool[8192];         /* 字符串字面量池（只读数据段） */
int nstrpool; int strpool_base;

char tok[256];              /* 当前 token 缓冲 */
int toklen; int tok_is_word; int tok_is_num; int tok_is_str; int tok_is_char;

int SYM_MAX = 512; int PATCH_MAX = 4096; int LAB_MAX = 4096;
int skind[512]; int sty[512]; int sbty[512]; int slen[512]; int sval[512]; int sname[512];
int nsym;
int pk[4096]; int ppos[4096]; int pname[4096];
int npatch;
int lpos[4096]; int lkind[4096]; int ltarget[4096];
int nlab;

int CODE_BASE = 0x800a0000; /* APP_LINK（hex 字面量，int 可载） */
int cur_nargs; int cur_frame; int frame_patch;
int code_cap;

char* code; int code_len;   /* 产物缓冲（brk 分配，char* 字节流） */
char* in; int in_len; int src_pos; int src_len;
int bty_top; int len_top;

/* ---- 节点/类型/符号常量（全局 int 变量初始化 = 常量） ---- */
int K_FUNC = 0; int K_GLOBAL = 1; int K_LOCAL = 2; int K_ARG = 3;
int TY_INT = 0; int TY_CHAR = 1; int TY_PTR = 2; int TY_ARRAY = 3;
int P_CALL = 0; int P_ADDR = 1;
int L_COND = 0; int L_JMP = 1;
int ND_NUM = 0; int ND_STR = 1; int ND_VAR = 2; int ND_FUNCALL = 3;
int ND_ADD = 4; int ND_SUB = 5; int ND_MUL = 6; int ND_DIV = 7; int ND_MOD = 8;
int ND_BITAND = 9; int ND_BITOR = 10; int ND_BITXOR = 11;
int ND_SHL = 12; int ND_SHR = 13;
int ND_EQ = 14; int ND_NE = 15; int ND_LT = 16; int ND_LE = 17; int ND_GT = 18; int ND_GE = 19;
int ND_AND = 20; int ND_OR = 21; int ND_NEG = 22; int ND_NOT = 23; int ND_BNOT = 24;
int ND_ASSIGN = 25;
int ND_ADDR = 26; int ND_DEREF = 27;
int ND_INDEX = 28;
int ND_EXPR_STMT = 29; int ND_BLOCK = 30; int ND_IF = 31; int ND_WHILE = 32; int ND_FOR = 33; int ND_RET = 34;
int ND_DECL = 35; int ND_FUNC = 36; int ND_GVAR = 37;

int print_num(int v) {
    char buf[16]; int bi = 0; int i;
    if (v < 0) { sys_print("-"); v = 0 - v; }
    while (v > 0) { buf[bi] = '0' + v % 10; bi = bi + 1; v = v / 10; }
    if (bi == 0) { buf[bi] = '0'; bi = 1; }
    i = bi;
    while (i > 0) {
        char t[2];
        i = i - 1;
        t[0] = buf[i]; t[1] = 0;
        sys_print(&t[0]);
    }
    return 0;
}

int fail(char* msg) {
    sys_print("minicc: error: ");
    sys_print(msg);
    sys_print(" [");
    sys_print(&tok[0]);
    sys_print("] @");
    print_num(src_pos);
    sys_print(" len="); print_num(src_len);
    sys_print("\n");
    syscall3(0, 1, 0, 0);
    return 1;
}

int xmalloc(int n) {
    int old = syscall3(35, 0, 0, 0);
    if (syscall3(35, old + n, 0, 0) != 0) fail("out of memory");
    return old;
}

/* ---- 字符串工具（名字池） ---- */
int seq_tok(char* s) {          /* tok 与字面量相等 */
    int i = 0;
    while (tok[i] && *(s+i)) { if (tok[i] != *(s+i)) return 0; i = i + 1; }
    if (tok[i] != *(s+i)) return 0;
    return 1;
}

int seq(int off, char* s) {     /* strtab[off] 与字面量相等 */
    int i = 0;
    while (strtab[off + i] && *(s+i)) {
        if (strtab[off + i] != *(s+i)) return 0;
        i = i + 1;
    }
    if (strtab[off + i] != *(s+i)) return 0;
    return 1;
}

int stradd(char* s) {           /* 拷贝入名字池，返回偏移 */
    int off = nstr;
    int i = 0;
    while (*(s+i)) { strtab[nstr] = *(s+i); nstr = nstr + 1; i = i + 1; }
    strtab[nstr] = 0; nstr = nstr + 1;
    return off;
}

/* ---- 输出码流 ---- */
int emit1(int b) {
    if (code_len >= code_cap) fail("output too big");
    *(code + code_len) = b;
    code_len = code_len + 1;
    return 0;
}

int emit4(int v) {
    emit1(v & 255); emit1((v >> 8) & 255);
    emit1((v >> 16) & 255); emit1((v >> 24) & 255);
    return 0;
}

int emit_op(char* s) {          /* 字符串字面量字节序列（\xNN 已在词法解码） */
    int i = 0;
    while (*(s+i)) { emit1(*(s+i)); i = i + 1; }
    return 0;
}

int save32(int pos, int v) {
    *(code + pos) = v & 255; *(code + pos + 1) = (v >> 8) & 255;
    *(code + pos + 2) = (v >> 16) & 255; *(code + pos + 3) = (v >> 24) & 255;
    return 0;
}

int peek() {
    /* 无符号读取（& 255）：源码含 UTF-8 非 ASCII 注释字节（>=0x80），
     * 有符号 char 会变负值被误判 EOF；minicc 无 cast，用位与替代 */
    if (src_pos < src_len) return *(in + src_pos) & 255;
    return -1;
}
int peek2() {
    if (src_pos + 1 < src_len) return *(in + src_pos + 1) & 255;
    return -1;
}

int decode_escape() {
    int e = peek();
    if (e < 0 || e == 10) fail("bad escape");
    src_pos = src_pos + 1;
    if (e == 'n') return 10;
    if (e == 't') return 9;
    if (e == '\\') return '\\';
    if (e == '"') return '"';
    if (e == '\'') return '\'';
    if (e == '0') return 0;
    if (e == 'x') {
        int v = 0; int got = 0; int k = 0;
        while (k < 2) {
            int h = peek();
            int ok = 0;
            if (h >= '0' && h <= '9') { v = v * 16 + (h - '0'); ok = 1; }
            else if (h >= 'a' && h <= 'f') { v = v * 16 + (h - 'a' + 10); ok = 1; }
            else if (h >= 'A' && h <= 'F') { v = v * 16 + (h - 'A' + 10); ok = 1; }
            if (ok == 0) k = 2;
            else { src_pos = src_pos + 1; got = 1; k = k + 1; }
        }
        if (got == 0) fail("bad \\x escape");
        return v;
    }
    fail("bad escape");
    return 0;
}

int next_tok() {
    tok_is_word = 0; tok_is_num = 0; tok_is_str = 0; tok_is_char = 0;
    int skip = 1;
    while (skip) {
        int c = peek();
        if (c == ' ' || c == 9 || c == 10 || c == 13) src_pos = src_pos + 1;
        else if (c == '/' && peek2() == '/') {
            src_pos = src_pos + 2;
            while (peek() != 10 && peek() >= 0) src_pos = src_pos + 1;
        }
        else if (c == '/' && peek2() == '*') {
            src_pos = src_pos + 2;
            int done = 0;
            while (done == 0) {
                if (peek() < 0) fail("unterminated comment");
                if (peek() == '*' && peek2() == '/') { src_pos = src_pos + 2; done = 1; }
                else src_pos = src_pos + 1;
            }
        }
        else skip = 0;
    }
    if (peek() < 0) { tok[0] = 0; toklen = 0; return 0; }
    int c = *(in + src_pos) & 255;
    src_pos = src_pos + 1;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int n = 0;
        tok[n] = c; n = n + 1;
        int more = 1;
        while (n < 255 && more) {
            int d = peek();
            if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                (d >= '0' && d <= '9') || d == '_') { tok[n] = d; n = n + 1; src_pos = src_pos + 1; }
            else more = 0;
        }
        tok[n] = 0; toklen = n; tok_is_word = 1;
        return;
    }
    if (c >= '0' && c <= '9') {
        int n = 0;
        tok[n] = c; n = n + 1;
        if (c == '0' && (peek() == 'x' || peek() == 'X')) {
            tok[n] = *(in + src_pos) & 255; n = n + 1; src_pos = src_pos + 1;
            int more = 1;
            while (n < 255 && more) {
                int d = peek();
                if ((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') ||
                    (d >= 'A' && d <= 'F')) { tok[n] = d; n = n + 1; src_pos = src_pos + 1; }
                else more = 0;
            }
            tok[n] = 0;
            if (peek() == '_' || (peek() >= '0' && peek() <= '9') ||
                (peek() >= 'a' && peek() <= 'z') || (peek() >= 'A' && peek() <= 'Z'))
                fail("bad number");
            toklen = n; tok_is_num = 1;
            return;
        }
        int more = 1;
        while (n < 255 && more) {
            int d = peek();
            if (d >= '0' && d <= '9') { tok[n] = d; n = n + 1; src_pos = src_pos + 1; }
            else more = 0;
        }
        tok[n] = 0;
        if (peek() == '_' || (peek() >= 'a' && peek() <= 'z') ||
            (peek() >= 'A' && peek() <= 'Z'))
            fail("bad number");
        toklen = n; tok_is_num = 1;
        return;
    }
    if (c == '"') {
        int n = 0;
        int done = 0;
        while (done == 0) {
            int d = peek();
            if (d < 0 || d == 10) fail("unterminated string");
            if (d == '"') { src_pos = src_pos + 1; done = 1; }
            else {
                src_pos = src_pos + 1;
                if (d == '\\') d = decode_escape();
                if (n >= 254) fail("string too long");
                tok[n] = d; n = n + 1;
            }
        }
        tok[n] = 0; toklen = n; tok_is_str = 1;
        return;
    }
    if (c == '\'') {
        int d = peek();
        if (d < 0 || d == 10) fail("unterminated char");
        src_pos = src_pos + 1;
        if (d == '\\') d = decode_escape();
        if (peek() != '\'') fail("char literal too long");
        src_pos = src_pos + 1;
        tok[0] = d; tok[1] = 0; toklen = 1; tok_is_char = 1;
        return;
    }
    int d = peek();
    if ((c == '=' && d == '=') || (c == '!' && d == '=') ||
        (c == '<' && d == '=') || (c == '>' && d == '=') ||
        (c == '<' && d == '<') || (c == '>' && d == '>') ||
        (c == '&' && d == '&') || (c == '|' && d == '|')) {
        tok[0] = c; tok[1] = d; tok[2] = 0;
        src_pos = src_pos + 1; toklen = 2;
        return;
    }
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
        c == '<' || c == '>' || c == '=' || c == '!' ||
        c == '&' || c == '|' || c == '^' || c == '~' ||
        c == '(' || c == ')' || c == '{' || c == '}' || c == ';' || c == ',') {
        tok[0] = c; tok[1] = 0; toklen = 1;
        return;
    }
    tok[0] = c; tok[1] = 0; toklen = 1;
}

/* 符号匹配：字面量 token（word/num/str/char）绝不参与符号比较，
 * 否则字符串 ")" / 字符 ';' 会被误判为括号/分号（BUG-034 同族）。 */
int is_sym_s(char* s) {
    if (tok_is_word || tok_is_num || tok_is_str || tok_is_char) return 0;
    return seq_tok(s);
}
int peek_s(char* s) {           /* 关键字/符号匹配：排除数字/字符串/字符字面量（word 保留给 if/int 等） */
    if (tok_is_num || tok_is_str || tok_is_char) return 0;
    return seq_tok(s);
}
int accept_s(char* s) {
    if (tok_is_num || tok_is_str || tok_is_char) return 0;
    if (seq_tok(s)) { next_tok(); return 1; }
    return 0;
}
int expect_s(char* s) { if (accept_s(s) == 0) fail("expected token"); return 0; }

/* ---- 符号表 / 补丁 / 标签 ---- */
int sym_find(int noff) {
    /* 从后往前（最近声明优先）：局部变量可遮蔽同名全局函数（如 finish 的局部 rel）。 */
    int i = nsym - 1;
    while (i >= 0) {
        if (seq(sname[i], &strtab[noff])) return i;
        i = i - 1;
    }
    return -1;
}

int sym_add(int noff, int kind, int ty, int bty, int len, int val) {
    if (nsym >= SYM_MAX) fail("symbol table full");
    sname[nsym] = noff; skind[nsym] = kind; sty[nsym] = ty;
    sbty[nsym] = bty; slen[nsym] = len; sval[nsym] = val;
    nsym = nsym + 1;
    return nsym - 1;
}

int patch_add(int noff, int pos, int kind) {
    if (npatch >= PATCH_MAX) fail("too many references");
    pname[npatch] = noff; ppos[npatch] = pos; pk[npatch] = kind;
    npatch = npatch + 1;
    return 0;
}

int new_lab() {
    if (nlab >= LAB_MAX) fail("too many labels");
    nlab = nlab + 1;
    return nlab;
}

int emit_cond(int op, int lab) {
    lpos[lab] = code_len; lkind[lab] = L_COND;
    emit1(0x0f); emit1(op); emit4(0);
    return 0;
}

int emit_jmp(int lab) {
    lpos[lab] = code_len; lkind[lab] = L_JMP;
    emit1(0xe9); emit4(0);
    return 0;
}

int emit_jmp_to(int target) {
    int p = code_len;
    emit1(0xe9);
    emit4(target - (p + 5));
    return 0;
}

int patch_lab(int lab, int target) { ltarget[lab] = target; return 0; }

int emit_mov_imm(int v) { emit1(0xb8); emit4(v); return 0; }
int emit_lea_ebp(int disp) { emit_op("\x8d\x85"); emit4(disp); return 0; }
int emit_load() { emit1(0x8b); emit1(0x00); return 0; }
int emit_load8() { emit1(0x0f); emit1(0xb6); emit1(0x00); return 0; }
int emit_store() { emit_op("\x5b\x89\x03"); return 0; }
int emit_store8() { emit_op("\x5b\x88\x03"); return 0; }
int emit_test() { emit_op("\x85\xc0"); return 0; }
int emit_epilogue() { emit_op("\x89\xec\x5d\xc3"); return 0; }

int emit_add_esp(int n4) {
    if (n4 <= 127) { emit_op("\x83\xc4"); emit1(n4); }
    else { emit_op("\x81\xc4"); emit4(n4); }
    return 0;
}

/* ---- 类型 ---- */
int decl_type(int* bty) {
    int base = TY_INT;
    if (accept_s("int")) base = TY_INT;
    else if (accept_s("char")) base = TY_CHAR;
    else fail("expected type");
    int stars = 0;
    while (accept_s("*")) stars = stars + 1;
    if (stars > 1) fail("unsupported: multi-level pointer");
    if (stars > 0) { *bty = base; return TY_PTR; }
    *bty = 0;
    return base;
}

int array_suffix(int ty, int* len) {
    *len = 0;
    if (accept_s("[") == 0) return 0;
    if (ty == TY_PTR) fail("unsupported: array of pointers");
    if (tok_is_num == 0) fail("array size must be a constant");
    int n = 0; int i = 0;
    while (tok[i]) { n = n * 10 + (tok[i] - '0'); i = i + 1; }
    if (n < 1) fail("array size must be positive");
    next_tok();
    expect_s("]");
    *len = n;
    return 1;
}

int size_of(int ty, int bty, int len) {
    if (ty == TY_ARRAY) {
        if (bty == TY_INT) return len * 4;
        return len;
    }
    return 4;
}

int type_eq(int t1, int b1, int t2, int b2) {
    if (t1 != t2) {
        if (t1 == TY_PTR && t2 == TY_INT) return 1;
        if (t1 == TY_PTR || t2 == TY_PTR) return 0;
        if (t1 == TY_ARRAY || t2 == TY_ARRAY) return 0;
        return 1;
    }
    if (t1 != TY_PTR && t1 != TY_ARRAY) return 1;
    return b1 == b2;
}

/* ---- AST ---- */
int node_new(int kind) {
    /* 数组 int[NMAX] 有效索引 0..NMAX-1：新句柄 nn+1 最大 NMAX-1，故守卫用 NMAX-1
     * （否则 nn 达 NMAX 时 nnlocals[NMAX]=0 会写穿到相邻全局 nn，把节点池清零——BUG-038） */
    if (nn >= NMAX - 1) fail("too many nodes");
    nn = nn + 1;
    nkind[nn] = kind; nty[nn] = TY_INT; nbty[nn] = 0; nlen[nn] = 0;
    nl[nn] = 0; nr[nn] = 0; na[nn] = 0; nb[nn] = 0; nnext[nn] = 0;
    nval[nn] = 0; nvkind[nn] = 0; nvslot[nn] = 0; nival[nn] = 0;
    nnargs[nn] = 0; nnlocals[nn] = 0;
    return nn;
}

/* ---- 语法分析（建树，返回节点句柄） ---- */
/* 说明：函数互调靠 minicc 隐式声明 + patch 收集，无需原型声明 */

int primary() {
    int n;
    if (tok_is_num) {
        n = node_new(ND_NUM);
        nval[n] = 0;
        if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
            int i = 2;
            while (tok[i]) {
                int d = tok[i]; int v;
                if (d >= '0' && d <= '9') v = d - '0';
                else if (d >= 'a' && d <= 'f') v = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') v = d - 'A' + 10;
                else fail("bad number");
                nval[n] = nval[n] * 16 + v;
                i = i + 1;
            }
        } else {
            int i = 0;
            while (tok[i]) {
                if (tok[i] < '0' || tok[i] > '9') fail("bad number");
                nval[n] = nval[n] * 10 + (tok[i] - '0');
                i = i + 1;
            }
        }
        next_tok();
        return n;
    }
    if (tok_is_char) {
        n = node_new(ND_NUM);
        nval[n] = tok[0];
        next_tok();
        return n;
    }
    if (tok_is_str) {
        n = node_new(ND_STR);
        nty[n] = TY_PTR; nbty[n] = TY_CHAR;
        nival[n] = nstrpool;
        int i = 0;
        while (i < toklen) { strpool[nstrpool] = tok[i]; nstrpool = nstrpool + 1; i = i + 1; }
        strpool[nstrpool] = 0; nstrpool = nstrpool + 1;
        next_tok();
        return n;
    }
    if (tok_is_word) {
        int noff = stradd(&tok[0]);
        next_tok();
        if (seq_tok("(")) {
            n = node_new(ND_FUNCALL);
            nival[n] = noff;
            int si = sym_find(noff);
            if (si >= 0 && skind[si] != K_FUNC) fail("call to non-function");
            if (si < 0) sym_add(noff, K_FUNC, TY_INT, 0, 0, -1);
            nval[n] = si;
            if (si < 0) nval[n] = nsym - 1;
            next_tok();
            int head = 0; int tail = 0;
            while (is_sym_s(")") == 0) {
                int arg = expr();
                if (head == 0) head = arg; else nnext[tail] = arg;
                tail = arg;
                nnargs[n] = nnargs[n] + 1;
                if (is_sym_s(",") != 0) next_tok();
            }
            next_tok();
            na[n] = head;
            return n;
        }
        int si = sym_find(noff);
        if (si < 0) fail("undefined variable");
        if (sty[si] == TY_ARRAY) {
            n = node_new(ND_INDEX);
            nl[n] = node_new(ND_VAR);
            nty[nl[n]] = TY_ARRAY; nbty[nl[n]] = sbty[si];
            nvkind[nl[n]] = skind[si]; nvslot[nl[n]] = sval[si];
            if (nvkind[nl[n]] == K_GLOBAL) nival[nl[n]] = noff;
            if (accept_s("[") == 0) fail("array without subscript");
            nr[n] = expr();
            if (nty[nr[n]] == TY_PTR || nty[nr[n]] == TY_ARRAY) fail("array index must be integer");
            expect_s("]");
            nty[n] = sbty[si];
            return n;
        }
        n = node_new(ND_VAR);
        nty[n] = sty[si]; nbty[n] = sbty[si];
        nvkind[n] = skind[si]; nvslot[n] = sval[si];
        if (nvkind[n] == K_GLOBAL) nival[n] = noff;
        return n;
    }
    if (accept_s("(")) {
        n = expr();
        expect_s(")");
        return n;
    }
    fail("bad expression");
    return 0;
}

int unary() {
    if (accept_s("-")) {
        int n = node_new(ND_NEG);
        nl[n] = unary();
        return n;
    }
    if (accept_s("!")) {
        int n = node_new(ND_NOT);
        nl[n] = unary();
        return n;
    }
    if (accept_s("~")) {
        int n = node_new(ND_BNOT);
        nl[n] = unary();
        return n;
    }
    if (accept_s("&")) {
        int n = node_new(ND_ADDR);
        nl[n] = unary();
        if (nkind[nl[n]] != ND_VAR && nkind[nl[n]] != ND_DEREF && nkind[nl[n]] != ND_INDEX)
            fail("cannot take address");
        nty[n] = TY_PTR;
        nbty[n] = nty[nl[n]];
        return n;
    }
    if (accept_s("*")) {
        int n = node_new(ND_DEREF);
        nl[n] = unary();
        if (nty[nl[n]] != TY_PTR) fail("dereference of non-pointer");
        nty[n] = nbty[nl[n]];
        return n;
    }
    return primary();
}

int bin(int l, int r, int kind) {
    int n = node_new(kind);
    nl[n] = l; nr[n] = r;
    if ((kind == ND_ADD || kind == ND_SUB) &&
        (nty[l] == TY_PTR || nty[r] == TY_PTR)) {
        nty[n] = TY_PTR;
        if (nty[l] == TY_PTR) nbty[n] = nbty[l]; else nbty[n] = nbty[r];
    }
    return n;
}

int mul() {
    int n = unary();
    while (1) {
        if (accept_s("*")) n = bin(n, unary(), ND_MUL);
        else if (accept_s("/")) n = bin(n, unary(), ND_DIV);
        else if (accept_s("%")) n = bin(n, unary(), ND_MOD);
        else return n;
    }
}

int add() {
    int n = mul();
    while (1) {
        if (accept_s("+")) n = bin(n, mul(), ND_ADD);
        else if (accept_s("-")) n = bin(n, mul(), ND_SUB);
        else return n;
    }
}

int shift() {
    int n = add();
    while (1) {
        if (accept_s("<<")) n = bin(n, add(), ND_SHL);
        else if (accept_s(">>")) n = bin(n, add(), ND_SHR);
        else return n;
    }
}

int rel() {
    int n = shift();
    while (1) {
        if (accept_s("<")) n = bin(n, shift(), ND_LT);
        else if (accept_s("<=")) n = bin(n, shift(), ND_LE);
        else if (accept_s(">")) n = bin(n, shift(), ND_GT);
        else if (accept_s(">=")) n = bin(n, shift(), ND_GE);
        else return n;
    }
}

int eq() {
    int n = rel();
    while (1) {
        if (accept_s("==")) n = bin(n, rel(), ND_EQ);
        else if (accept_s("!=")) n = bin(n, rel(), ND_NE);
        else return n;
    }
}

int bitand() {
    int n = eq();
    while (accept_s("&")) n = bin(n, eq(), ND_BITAND);
    return n;
}

int bitxor() {
    int n = bitand();
    while (accept_s("^")) n = bin(n, bitand(), ND_BITXOR);
    return n;
}

int bitor() {
    int n = bitxor();
    while (accept_s("|")) n = bin(n, bitxor(), ND_BITOR);
    return n;
}

int land() {
    int n = bitor();
    while (accept_s("&&")) n = bin(n, bitor(), ND_AND);
    return n;
}

int lor() {
    int n = land();
    while (accept_s("||")) n = bin(n, land(), ND_OR);
    return n;
}

int expr() {
    int n = lor();
    if (accept_s("=")) {
        int a = node_new(ND_ASSIGN);
        nl[a] = n; nr[a] = expr();
        if (nkind[nl[a]] != ND_VAR && nkind[nl[a]] != ND_DEREF && nkind[nl[a]] != ND_INDEX)
            fail("assign to non-lvalue");
        if (type_eq(nty[nl[a]], nbty[nl[a]], nty[nr[a]], nbty[nr[a]]) == 0)
            fail("type mismatch in assignment");
        return a;
    }
    return n;
}

int block_stmt() {
    int mark = nsym;
    int head = 0; int tail = 0;
    while (is_sym_s("}") == 0) {
        if (tok[0] == 0) fail("unexpected end of file");
        int s = stmt();
        if (head == 0) head = s; else nnext[tail] = s;
        tail = s;
    }
    next_tok();
    nsym = mark;
    int n = node_new(ND_BLOCK);
    na[n] = head;
    return n;
}

int stmt() {
    int n;
    if (is_sym_s("{")) {
        next_tok();
        return block_stmt();
    }
    if (peek_s("int") || peek_s("char")) {
        n = node_new(ND_DECL);
        nty[n] = decl_type(&nbty[n]);
        if (tok_is_word == 0) fail("expected identifier");
        int noff = stradd(&tok[0]);
        next_tok();
        if (array_suffix(nty[n], &nlen[n])) { nbty[n] = nty[n]; nty[n] = TY_ARRAY; }
        if (nty[n] == TY_ARRAY && peek_s("=")) fail("array init not supported");
        int size = size_of(nty[n], nbty[n], nlen[n]);
        cur_frame = cur_frame + size;
        if (cur_frame > 4096) fail("frame too big");
        nval[n] = cur_frame;
        sym_add(noff, K_LOCAL, nty[n], nbty[n], nlen[n], nval[n]);
        if (accept_s("=")) {
            nl[n] = expr();
            if (type_eq(nty[n], nbty[n], nty[nl[n]], nbty[nl[n]]) == 0)
                fail("type mismatch in initialization");
        }
        expect_s(";");
        return n;
    }
    if (accept_s("if")) {
        expect_s("(");
        n = node_new(ND_IF);
        nl[n] = expr();
        expect_s(")");
        nr[n] = stmt();
        if (accept_s("else")) nb[n] = stmt();
        return n;
    }
    if (accept_s("while")) {
        expect_s("(");
        n = node_new(ND_WHILE);
        nl[n] = expr();
        expect_s(")");
        nr[n] = stmt();
        return n;
    }
    if (accept_s("for")) {
        expect_s("(");
        n = node_new(ND_FOR);
        if (peek_s(";") == 0) nl[n] = expr();
        expect_s(";");
        if (peek_s(";") == 0) nr[n] = expr();
        expect_s(";");
        if (peek_s(")") == 0) na[n] = expr();
        expect_s(")");
        nb[n] = stmt();
        return n;
    }
    if (accept_s("return")) {
        n = node_new(ND_RET);
        if (peek_s(";") == 0) nl[n] = expr();
        expect_s(";");
        return n;
    }
    n = node_new(ND_EXPR_STMT);
    nl[n] = expr();
    expect_s(";");
    return n;
}

int funcs; int funcs_tail; int gvars; int gvars_tail;

int parse_program() {
    while (1) {
        if (tok[0] == 0) return 0;
        if (peek_s("int") == 0 && peek_s("char") == 0) fail("expected type");
        int ty = decl_type(&bty_top);
        if (tok_is_word == 0) fail("expected identifier");
        int noff = stradd(&tok[0]);
        next_tok();
        len_top = 0;
        if (array_suffix(ty, &len_top)) { bty_top = ty; ty = TY_ARRAY; }
        if (accept_s("(")) {
            int si = sym_find(noff);
            if (si >= 0) {
                if (skind[si] != K_FUNC || sval[si] >= 0) fail("redefined");
            } else {
                si = sym_add(noff, K_FUNC, TY_INT, 0, 0, -1);
            }
            int fn = node_new(ND_FUNC);
            nival[fn] = noff;
            nval[fn] = si;
            int func_scope = nsym;
            cur_nargs = 0; cur_frame = 0;
            int params = 0; int ptail = 0;
            if (is_sym_s(")") == 0) {
                int more = 1;
                while (more) {
                    int p = node_new(ND_VAR);
                    nty[p] = decl_type(&nbty[p]);
                    if (tok_is_word == 0) fail("expected parameter name");
                    nival[p] = stradd(&tok[0]);
                    next_tok();
                    int plen;
                    if (array_suffix(nty[p], &plen)) fail("unsupported: array parameter");
                    nvkind[p] = K_ARG;
                    nvslot[p] = cur_nargs;
                    sym_add(nival[p], K_ARG, nty[p], nbty[p], 0, cur_nargs);
                    if (params == 0) params = p; else nnext[ptail] = p;
                    ptail = p;
                    cur_nargs = cur_nargs + 1;
                    if (accept_s(",") != 0) more = 1; else more = 0;
                }
            }
            expect_s(")");
            nnargs[fn] = cur_nargs;
            na[fn] = params;
            if (accept_s("{") == 0) fail("expected function body");
            nb[fn] = block_stmt();
            nnlocals[fn] = cur_frame;
            nsym = func_scope;
            if (funcs == 0) funcs = fn; else nnext[funcs_tail] = fn;
            funcs_tail = fn;
        } else {
            if (sym_find(noff) >= 0) fail("redefined");
            int g = node_new(ND_GVAR);
            nival[g] = noff;
            nty[g] = ty; nbty[g] = bty_top; nlen[g] = len_top;
            int si = sym_add(noff, K_GLOBAL, ty, bty_top, len_top, 0);
            nval[g] = si;
            if (accept_s("=")) {
                if (ty == TY_ARRAY) fail("array init not supported");
                /* 标量初值存 nlen[g]（数组长度与初值互斥：标量 len_top=0）。
                 * 支持十六进制（CODE_BASE = 0x800a0000）；十进制环原样——BUG-039：
                 * 旧十进制环把 'x'/'a' 当数字位算，0x800a0000 变垃圾，P1 运行时
                 * CODE_BASE 错乱致自举 P1 != P2（字符串/全局寻址全偏）。 */
                if (tok_is_num) {
                    nlen[g] = 0;
                    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
                        int i = 2;
                        while (tok[i]) {
                            int d = tok[i]; int v = 0;
                            if (d >= '0' && d <= '9') v = d - '0';
                            else if (d >= 'a' && d <= 'f') v = d - 'a' + 10;
                            else if (d >= 'A' && d <= 'F') v = d - 'A' + 10;
                            else fail("bad hex global init");
                            nlen[g] = nlen[g] * 16 + v;
                            i = i + 1;
                        }
                    } else {
                        int i = 0;
                        while (tok[i]) { nlen[g] = nlen[g] * 10 + (tok[i] - '0'); i = i + 1; }
                    }
                    next_tok();
                } else if (tok_is_char) {
                    nlen[g] = tok[0];
                    next_tok();
                } else {
                    fail("global init must be a constant");
                }
            }
            expect_s(";");
            if (gvars == 0) gvars = g; else nnext[gvars_tail] = g;
            gvars_tail = g;
        }
    }
}

/* ---- 代码生成 ---- */
/* gen_addr 在解引用/下标时递归调用 gen（隐式声明） */

int gen_addr(int n) {
    if (nkind[n] == ND_VAR) {
        if (nvkind[n] == K_LOCAL) emit_lea_ebp(0 - nvslot[n]);
        else if (nvkind[n] == K_ARG) emit_lea_ebp(8 + 4 * (cur_nargs - 1 - nvslot[n]));
        else if (nvkind[n] == K_GLOBAL) {
            emit_mov_imm(0);
            patch_add(nival[n], code_len - 4, P_ADDR);
        } else fail("not a variable");
        return 0;
    }
    if (nkind[n] == ND_DEREF) {
        gen(nl[n]);
        return 0;
    }
    if (nkind[n] == ND_INDEX) {
        gen_addr(nl[n]);
        emit1(0x50);
        gen(nr[n]);
        if (nbty[nl[n]] == TY_INT) emit_op("\xc1\xe0\x02");
        emit_op("\x5b\x01\xd8");
        return 0;
    }
    fail("assign to non-lvalue");
    return 0;
}

int gen(int n) {
    if (nkind[n] == ND_NUM) { emit_mov_imm(nval[n]); return 0; }
    if (nkind[n] == ND_STR) {
        emit_mov_imm(CODE_BASE + strpool_base + nival[n]);
        return 0;
    }
    if (nkind[n] == ND_VAR || nkind[n] == ND_DEREF || nkind[n] == ND_INDEX) {
        gen_addr(n);
        if (nty[n] == TY_CHAR) emit_load8(); else emit_load();
        return 0;
    }
    if (nkind[n] == ND_ADDR) { gen_addr(nl[n]); return 0; }
    if (nkind[n] == ND_NEG) { gen(nl[n]); emit_op("\xf7\xd8"); return 0; }
    if (nkind[n] == ND_BNOT) { gen(nl[n]); emit_op("\xf7\xd0"); return 0; }
    if (nkind[n] == ND_NOT) {
        gen(nl[n]); emit_test();
        emit_op("\x0f\x94\xc0\x0f\xb6\xc0");
        return 0;
    }
    if (nkind[n] == ND_ASSIGN) {
        gen_addr(nl[n]); emit1(0x50);
        gen(nr[n]);
        if (nty[nl[n]] == TY_CHAR) emit_store8(); else emit_store();
        return 0;
    }
    if (nkind[n] == ND_BITAND) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x21\xd8"); return 0; }
    if (nkind[n] == ND_BITOR) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x09\xd8"); return 0; }
    if (nkind[n] == ND_BITXOR) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x31\xd8"); return 0; }
    if (nkind[n] == ND_SHL) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x87\xd8\x89\xd9\xd3\xe0"); return 0; }
    if (nkind[n] == ND_SHR) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x87\xd8\x89\xd9\xd3\xf8"); return 0; }
    if (nkind[n] == ND_ADD) {
        gen(nl[n]); emit1(0x50); gen(nr[n]);
        if (nty[nl[n]] == TY_PTR && nty[nr[n]] != TY_PTR) {
            if (nbty[nl[n]] == TY_INT) emit_op("\xc1\xe0\x02");
        } else if (nty[nl[n]] != TY_PTR && nty[nr[n]] == TY_PTR) {
            if (nbty[nr[n]] == TY_INT) emit_op("\xc1\xe3\x02");
        }
        emit_op("\x5b\x01\xd8");
        return 0;
    }
    if (nkind[n] == ND_SUB) {
        gen(nl[n]); emit1(0x50); gen(nr[n]);
        if (nty[nl[n]] == TY_PTR && nty[nr[n]] != TY_PTR) {
            if (nbty[nl[n]] == TY_INT) emit_op("\xc1\xe0\x02");
        } else if (nty[nr[n]] == TY_PTR) fail("invalid pointer subtraction");
        emit_op("\x5b\x29\xc3\x89\xd8");
        return 0;
    }
    if (nkind[n] == ND_MUL) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x0f\xaf\xc3"); return 0; }
    if (nkind[n] == ND_DIV) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x87\xd8\x99\xf7\xfb"); return 0; }
    if (nkind[n] == ND_MOD) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x87\xd8\x99\xf7\xfb\x89\xd0"); return 0; }
    if (nkind[n] == ND_LT) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x39\xc3\x0f\x9c\xc0\x0f\xb6\xc0"); return 0; }
    if (nkind[n] == ND_LE) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x39\xc3\x0f\x9e\xc0\x0f\xb6\xc0"); return 0; }
    if (nkind[n] == ND_GT) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x39\xc3\x0f\x9f\xc0\x0f\xb6\xc0"); return 0; }
    if (nkind[n] == ND_GE) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x39\xc3\x0f\x9d\xc0\x0f\xb6\xc0"); return 0; }
    if (nkind[n] == ND_EQ) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x39\xc3\x0f\x94\xc0\x0f\xb6\xc0"); return 0; }
    if (nkind[n] == ND_NE) { gen(nl[n]); emit1(0x50); gen(nr[n]); emit_op("\x5b\x39\xc3\x0f\x95\xc0\x0f\xb6\xc0"); return 0; }
    if (nkind[n] == ND_AND) {
        int fa1 = new_lab(); int fa2 = new_lab(); int en = new_lab();
        gen(nl[n]); emit_test(); emit_cond(0x84, fa1);
        gen(nr[n]); emit_test(); emit_cond(0x84, fa2);
        emit_mov_imm(1);
        emit_jmp(en);
        patch_lab(fa1, code_len);
        patch_lab(fa2, code_len);
        emit_op("\x31\xc0");
        patch_lab(en, code_len);
        return 0;
    }
    if (nkind[n] == ND_OR) {
        int tr1 = new_lab(); int tr2 = new_lab(); int en = new_lab();
        gen(nl[n]); emit_test(); emit_cond(0x85, tr1);
        gen(nr[n]); emit_test(); emit_cond(0x85, tr2);
        emit_op("\x31\xc0");
        emit_jmp(en);
        patch_lab(tr1, code_len);
        patch_lab(tr2, code_len);
        emit_mov_imm(1);
        patch_lab(en, code_len);
        return 0;
    }
    if (nkind[n] == ND_FUNCALL) {
        int a = na[n];
        while (a != 0) {
            gen(a);
            emit1(0x50);
            a = nnext[a];
        }
        emit1(0xe8); emit4(0);
        patch_add(nival[n], code_len - 4, P_CALL);
        emit_add_esp(nnargs[n] * 4);
        return 0;
    }
    fail("internal: bad expr node");
    return 0;
}

int gen_stmt(int n) {
    if (nkind[n] == ND_EXPR_STMT) { gen(nl[n]); return 0; }
    if (nkind[n] == ND_BLOCK) {
        int s = na[n];
        while (s != 0) { gen_stmt(s); s = nnext[s]; }
        return 0;
    }
    if (nkind[n] == ND_DECL) {
        if (nl[n] != 0) {
            emit_lea_ebp(0 - nval[n]);
            emit1(0x50);
            gen(nl[n]);
            if (nty[n] == TY_CHAR) emit_store8(); else emit_store();
        }
        return 0;
    }
    if (nkind[n] == ND_IF) {
        int els = new_lab(); int en = new_lab();
        gen(nl[n]); emit_test(); emit_cond(0x84, els);
        gen_stmt(nr[n]);
        emit_jmp(en);
        patch_lab(els, code_len);
        if (nb[n] != 0) gen_stmt(nb[n]);
        patch_lab(en, code_len);
        return 0;
    }
    if (nkind[n] == ND_WHILE) {
        int top = code_len;
        int en = new_lab();
        gen(nl[n]); emit_test(); emit_cond(0x84, en);
        gen_stmt(nr[n]);
        emit_jmp_to(top);
        patch_lab(en, code_len);
        return 0;
    }
    if (nkind[n] == ND_FOR) {
        if (nl[n] != 0) gen(nl[n]);
        int top = code_len;
        int en = new_lab();
        if (nr[n] != 0) { gen(nr[n]); emit_test(); emit_cond(0x84, en); }
        gen_stmt(nb[n]);
        if (na[n] != 0) gen(na[n]);
        emit_jmp_to(top);
        patch_lab(en, code_len);
        return 0;
    }
    if (nkind[n] == ND_RET) {
        if (nl[n] != 0) gen(nl[n]);
        emit_epilogue();
        return 0;
    }
    sys_print("badstmt kind="); print_num(nkind[n]); sys_print(" n="); print_num(n); sys_print("\n");
    fail("internal: bad stmt node");
    return 0;
}

int gen_global(int n) {
    int si = nval[n];
    int pos = code_len;
    int size = size_of(nty[n], nbty[n], nlen[n]);
    int i = 0;
    while (i < size) { emit1(0); i = i + 1; }
    /* 标量常量初始化（数组仅 0 填充）；初值存于 nlen[g]（与数组长度互斥） */
    if (nty[n] != TY_ARRAY && nlen[n] != 0) save32(pos, nlen[n]);
    sval[si] = pos;
    return 0;
}

int gen_func(int n) {
    int si = nval[n];
    sval[si] = code_len;
    cur_nargs = nnargs[n];
    emit_op("\x55\x89\xe5");
    emit_op("\x81\xec"); emit4(0);
    frame_patch = code_len - 4;
    gen_stmt(nb[n]);
    emit_epilogue();
    save32(frame_patch, nnlocals[n]);
    return 0;
}

int finish() {
    int i = 0;
    while (i < nlab) {
        int lab = i + 1;
        int imm;
        if (lkind[lab] == L_COND) imm = lpos[lab] + 2; else imm = lpos[lab] + 1;
        int rel;
        if (lkind[lab] == L_COND) rel = ltarget[lab] - (lpos[lab] + 6);
        else rel = ltarget[lab] - (lpos[lab] + 5);
        save32(imm, rel);
        i = i + 1;
    }
    i = 0;
    while (i < npatch) {
        int si = sym_find(pname[i]);
        if (pk[i] == P_CALL) {
            if (si < 0 || skind[si] != K_FUNC || sval[si] < 0)
                fail("undefined function");
            save32(ppos[i], sval[si] - (ppos[i] + 4));
        } else {
            if (si < 0 || skind[si] != K_GLOBAL) fail("internal: bad addr patch");
            save32(ppos[i], CODE_BASE + sval[si]);
        }
        i = i + 1;
    }
    save32(68, code_len);
    save32(72, code_len);
    return 0;
}

/* ---- 文件 I/O（SYS_PRINT=1 FS_CREATE=13 FS_OPEN=14 FS_WRITE=15 FS_READ=16
 *        FS_CLOSE=17 FS_DELETE=19 BRK=35 EXIT=0） ---- */
int open_input(char* path) {
    if (syscall3(14, 1, path, 0) != 0) return -1;
    in_len = 0;
    int done = 0;
    while (done == 0) {
        if (in_len >= 60000) fail("input too big");
        int n = syscall3(16, 1, in + in_len, 4096);
        if (n <= 0) done = 1;
        else in_len = in_len + n;
    }
    syscall3(17, 1, 0, 0);
    return 0;
}

int write_output(char* path) {
    syscall3(19, path, 0, 0);
    if (syscall3(13, path, 0, 0) < 0) return -1;
    if (syscall3(14, 2, path, 1) != 0) return -1;
    int w = syscall3(15, 2, code, code_len);
    syscall3(17, 2, 0, 0);
    if (w == code_len) return 0;
    return -1;
}

/* ---- ELF32 头 + 入口 stub（95 字节，含内嵌 \x00，按长度逐字节 emit） ---- */
int emit_elf_header() {
    char* h = "\x7f\x45\x4c\x46\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x03\x00\x01\x00\x00\x00\x54\x00\x0a\x80\x34\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x34\x00\x20\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0a\x80\x00\x00\x0a\x80\x10\x4b\x00\x00\x10\x4b\x00\x00\x07\x00\x00\x00\x00\x10\x00\x00\xe8\x00\x00\x00\x00\x89\xc3\x31\xc0\xcd\x80";
    int i = 0;
    while (i < 95) { emit1(*(h+i)); i = i + 1; }
    return 0;
}

int main() {
    code_cap = 860000;
    code = xmalloc(860000);
    in = xmalloc(65536);
    code_len = 0;
    nsym = 0; npatch = 0; nlab = 0; nstr = 0; nn = 0; nstrpool = 0;
    funcs = 0; funcs_tail = 0; gvars = 0; gvars_tail = 0;
    if (open_input("/minicc.c") != 0) {
        sys_print("minicc: input open fail\n");
        return 1;
    }
    src_pos = 0; src_len = in_len;
    next_tok();
    parse_program();
    if (tok[0] != 0) fail("unexpected token");

    emit_elf_header();
    patch_add(stradd("main"), 0x55, P_CALL);
    strpool_base = code_len;
    int i = 0;
    while (i < nstrpool) { emit1(strpool[i]); i = i + 1; }
    int g = gvars;
    while (g != 0) { gen_global(g); g = nnext[g]; }
    int f = funcs;
    while (f != 0) { gen_func(f); f = nnext[f]; }
    i = 0;
    int done = 0;
    while (i < npatch && done == 0) {
        if (pk[i] == P_CALL && seq(pname[i], "syscall3")) {
            int ss = sym_find(pname[i]);
            if (ss < 0) ss = sym_add(pname[i], K_FUNC, TY_INT, 0, 0, -1);
            if (sval[ss] < 0) {
                sval[ss] = code_len;
                emit_op("\x55\x89\xe5");
                emit_op("\x8b\x45\x14");
                emit_op("\x8b\x5d\x10");
                emit_op("\x8b\x4d\x0c");
                emit_op("\x8b\x55\x08");
                emit1(0xcd); emit1(0x80);
                emit_op("\x5d\xc3");
            }
            done = 1;
        }
        i = i + 1;
    }
    finish();

    if (write_output("/out.elf") != 0) {
        sys_print("minicc: output write fail\n");
        return 1;
    }
    sys_print("minicc: compiled OK\n");
    return 0;
}

int minicc_main(int a, int b) { return main(); }
