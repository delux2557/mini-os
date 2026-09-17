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
 *     （此条约束的是"本实现自身怎么写"：词法器/解析器/生成器对新代码的书写禁律。
 *      编译器对外语言子集则与 minicc.c 完全对齐——V3b 的 ++/--（前/后缀）、复合赋值、
 *      do-while、break/continue 已在词法/解析/生成三层同步实现，见下。）
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

/* ---- 内存池 ----
 * V4 arena 瘦身：小枚举字段用 char（nkind/nty/nbty/nvkind/nnargs/nnlocals 值恒 <256；
 * 其余节点句柄/偏移仍须 int）。
 * #172：节点池 15 个并行数组**全部改为运行时 brk 分配**。原先它们是静态数组，而 minicc 不分离
 * .bss——零初始化数据按字面量全量内联进产物，于是"节点容量"直接等于"内核里那份自举编译器的
 * 体积"（42 B/节点）。后果是容量只能抠着给：旧配置 8192 只差 123 个节点，可抬容量会让内核涨
 * 数百 KB，于是谁都不动它、自举就这么静默断了。改为动态后产物只含代码与常量，容量与实际需求
 * 脱钩，可以一次给足余量。分配见 main()（必须在解析前完成）。 */

/* ---- 自举容量（#172：按源的**实测**体积定；改动前先读这段） ----
 * 下面几个常量决定"能自举多大的自己"。它们**不随源一起长**，故源涨过头必须同步抬——
 * 否则症状是自举静默失败：P1 读不下自己的源（input too big）、勉强读下却在解析末尾撞节点池
 * （too many nodes）、或产物超 code_cap（output too big）。2026-09 已这样断过一次，见 issue #172。
 *
 * 四条天花板的**代价**（决定了余量怎么给）：
 *   ① 输入 IN_CAP/IN_LIMIT：in 走 xmalloc ⇒ 对产物零代价；
 *   ② 节点池 NMAX：走 xmalloc ⇒ 对产物零代价（#172 前是静态数组，42 B/节点全进产物）；
 *   ③ 产物 code_cap：emit1 **不扩容** ⇒ 它是产物硬上限（缓冲容量本身不计入产物）；
 *   ④ 名字池 STRTAB_CAP：走 xmalloc ⇒ 对产物零代价（#172 前是静态 20 KB，也进产物）。
 *   ⇒ 四条里只有 ③ 与产物挂钩，其余只吃运行时内存，故可以给足。
 *
 * 定容依据（2026-09-17 实测，源 66121 B）：
 *   AST 节点峰值 = **8,809**（用 minicc_self 自己打点；宿主 minicc 同源 8,317 ⇒ 两份实现只差
 *     约 5%，**不是**初稿所称的"差一倍"）⇒ 密度 ≈ 0.133 节点/字节；
 *   名字池 nstr = **18,496**（≈ 源字节 × 0.28）——当时 strtab[20480] 的 **90%**，见下 ④；
 *   产物基线：节点池/名字池动态化后约 155 KB（此前 497 KB 里约 344 KB 是节点数组的零填充）。
 * ⚠ 初稿错在哪（留档以免重犯）：为测 NMAX 用了 `sed s/13312/N/g` 批量放大，而同一轮里 strtab
 *   已被误改成 13312 ⇒ **名字池被一起放大**，于是"16384 失败"其实是 **strtab 溢出写穿相邻全局**
 *   造成的"too many nodes"（一个与真因无关的报错）；据此推出的 0.269~0.315 高密度作废。
 * 取值：IN_LIMIT = 126976（源上限；密度 0.133 ⇒ 满源约需 17k 节点）；
 *   NMAX = 49152（≈5× 当前需求，动态分配故只吃运行时内存）；
 *   STRTAB_CAP = 131072（满源按 0.28 需 ≈36 KB ⇒ 3.6× 余量）；
 *   code_cap = 500000（≈155 KB 产物的 3 倍余量）。
 *
 * 门禁分工（改本段后必须同步核对）：
 *   ① mc_matrix 的 S1/S2a~S2e 钉：可编译子集、读块余量关系、源 ≤75% IN_LIMIT、
 *      **实测产物** < 0.9×code_cap、节点池未回退、名字池 ≥ 源 × 0.5（按 0.28 实测留足余量）；
 *   ② CI 的 miccboot 层：整链真编一次（**节点峰值只能实测**，两份实现的 AST 规模不可互推，
 *      故这一层不可省）。 */
int IN_CAP = 131072;        /* 输入缓冲容量（main 里 in 的 xmalloc 大小） */
int IN_LIMIT = 126976;      /* 可接受的最大源字节数 = IN_CAP - 4096（留一个读块余量） */
int NMAX = 49152;           /* AST 节点池容量（运行时分配，见上） */
char* nkind; char* nty; char* nbty; int* nlen;
int* nl; int* nr; int* na; int* nb; int* nnext;
int* nval; char* nvkind; int* nvslot; int* nival;
char* nnargs; char* nnlocals;
int nn;                     /* 当前节点数（0 保留为 NULL） */

int STRTAB_CAP = 131072;    /* 名字池容量（运行时分配，见上 ④） */
char* strtab;               /* 名字池（stradd 追加，NUL 终止；越界由守卫受控报错） */
int nstr;

char strpool[4096];         /* 字符串字面量池（只读数据段） */
int nstrpool; int strpool_base;

char tok[256];              /* 当前 token 缓冲 */
int toklen; int tok_is_word; int tok_is_num; int tok_is_str; int tok_is_char;

int SYM_MAX = 256; int PATCH_MAX = 2048; int LAB_MAX = 2048;
int skind[256]; int sty[256]; int sbty[256]; int slen[256]; int sval[256]; int sname[256];
int snargs[256]; /* FUNC 形参个数（未知=-1）——MC-04 arity 收敛（minicc.c FIX-G 同步） */
char sdef[256];  /* MC-08 末角（与 minicc.c Sym.defined 同步）：FUNC/GLOBAL 是否已给出定义
                    （parse 期即可判定；1=已定义）。sval 只在 codegen 期才非负，旧判定 parse 期恒假 */
int nsym;
int pk[2048]; int ppos[2048]; int pname[2048];
int npatch;
int lpos[2048]; int lkind[2048]; int ltarget[2048];
int nlab;
/* V3b 循环帧栈（与 minicc.c 同步）：ND_DO/WHILE/FOR 的 break/continue 未决跳转记录。
 * minicc 不支持多维数组声明，故以 [32 帧 * 64 槽] 线性展开；host 用 loop_brk[32][64]。 */
int loop_brk[2048]; int loop_cont[2048];
int loop_brk_n[32]; int loop_cont_n[32];
int nloop;

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
/* V3b（与 minicc.c 同步）：后缀 ++/--（值为旧值）、do-while、break/continue */
int ND_POST_INC = 38; int ND_POST_DEC = 39;
int ND_DO = 40; int ND_BREAK = 41; int ND_CONTINUE = 42;

/* MC-09 递归深度守卫（与 host minicc.c 同步；guest 用户栈仅 28KB，超限受控报错非 SIGSEGV）：
 *   EXPR_DEPTH_MAX 32  表达式/操作数嵌套（expr/unary 入口）
 *   STMT_DEPTH_MAX 128 语句/块嵌套（block_stmt 入口）
 *   GEN_DEPTH_MAX  256 codegen AST 深度（gen/gen_stmt 入口） */
int EXPR_DEPTH_MAX = 32; int STMT_DEPTH_MAX = 128; int GEN_DEPTH_MAX = 256;
int edepth; int sdepth; int gdepth;

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
    /* 有意架构差异（勿当 FIX-D 遗漏）：与 host 完整版 minicc.c 的 `char name[32]`（每符号定长大数组，
     * MC-02 要求词法拒绝 >31 字符标识符）不同，本自举版把名字统一拷入共享 strtab 名字池，无
     * 每符号固定大小数组，故 MC-02（写穿 name[32]）在结构上不适用、无需 identifier too long 检查。
     * host/guest 因此对 >31 字符标识符的接受性不同，属预期的行为分叉而非回归。 */
    /* #172 补正：**必须有越界守卫**。旧码 `strtab[nstr] = …` 没有任何比较——池子满时会静默
     * 写穿相邻全局（nstr/strpool/tok…，此处已是堆分配则写穿相邻堆块），把词法状态毁掉，报出来
     * 的却是"too many nodes"这种与真因无关的错。实测：镜像分支的 label 探测对每条以单词开头的
     * 语句都多入池一份名字（strtab 用量 18.5 KB/20.5 KB 已是 90%），撑爆后解析空转狂分配节点，
     * 排查全程被"too many nodes"误导。容量可以小，静默写穿不可接受。 */
    int off = nstr;
    int i = 0;
    for (;;) {
        if (nstr >= STRTAB_CAP - 1) fail("strtab full");
        if (*(s + i) == 0) break;
        strtab[nstr] = *(s + i);
        nstr = nstr + 1;
        i = i + 1;
    }
    strtab[nstr] = 0;
    nstr = nstr + 1;
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
        /* MC-08#5（与 minicc.c 同步）：\x 贪心吃全部十六进制位（C 语义），按 char 宽度
         * & 255 截断——旧实现固定 2 位把 "\x41F" 拆成 'A','F'（与 C 单一值 0x41F 不符）。
         * 上限 7 位防溢出；>28bit 显式失败收口。
         * 注意：不用 break（本自举源不支持 break/continue，stmt 仅 if/while/for/return）。 */
        int v = 0; int got = 0; int cont = 1;
        while (cont == 1) {
            int h = peek();
            int ok = 0;
            if (h >= '0' && h <= '9') { v = v * 16 + (h - '0'); ok = 1; }
            else if (h >= 'a' && h <= 'f') { v = v * 16 + (h - 'a' + 10); ok = 1; }
            else if (h >= 'A' && h <= 'F') { v = v * 16 + (h - 'A' + 10); ok = 1; }
            if (ok == 0) { cont = 0; }
            else { src_pos = src_pos + 1; got = got + 1;
                if (got > 7) fail("bad \\x escape"); }
        }
        if (got == 0) fail("bad \\x escape");
        return v & 255;
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
            if (n == 2) fail("empty hex literal");   /* FIX-E（同步）：0x 后无十六进制位 */
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
        /* FIX-E（同步）：minicc 不支持八进制，`010` 按十进制会被静默解成 10（C 应为 8）。拒绝前导 0 多位数字形态。 */
        if (tok[0] == '0' && tok[1] != 0) fail("octal literals not supported");
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
        (c == '&' && d == '&') || (c == '|' && d == '|') ||
        /* V3b（与 minicc.c 同步）：++ -- += -= *= /= %= */
        (c == '+' && (d == '+' || d == '=')) ||
        (c == '-' && (d == '-' || d == '=')) ||
        (c == '*' && d == '=') || (c == '/' && d == '=') ||
        (c == '%' && d == '=')) {
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
    if (kind == K_FUNC) snargs[nsym] = -1; else snargs[nsym] = 0;   /* FUNC 形参个数未知；其余无意义 */
    if (kind == K_FUNC) sdef[nsym] = 0; else sdef[nsym] = 1;        /* MC-08 末角（同步） */
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

/* V3b（与 minicc.c 同步）：直接目标的条件跳转（0F <op> rel32，向后/绝对目标用，不走 labs
 * 前向补丁）。指令长 6 字节，rel 相对"指令尾" target - (p+6)。 */
int emit_cond_direct(int op, int target) {
    int p = code_len;
    emit1(0x0f); emit1(op); emit4(target - (p + 6));
    return 0;
}

/* ---- V3b 循环 break/continue 目标栈（与 minicc.c loop_enter/loop_patch_* 同步） ---- */
int loop_enter() {
    if (nloop >= 32) fail("loop nesting too deep");
    loop_brk_n[nloop] = 0; loop_cont_n[nloop] = 0;
    nloop = nloop + 1;
    return 0;
}
int loop_leave() { if (nloop > 0) nloop = nloop - 1; return 0; }
int loop_brk_add() {
    int i = nloop - 1;
    if (loop_brk_n[i] >= 64) fail("too many break in a loop");
    loop_brk[i * 64 + loop_brk_n[i]] = code_len;
    loop_brk_n[i] = loop_brk_n[i] + 1;
    emit1(0xe9); emit4(0);
    return 0;
}
int loop_cont_add() {
    int i = nloop - 1;
    if (loop_cont_n[i] >= 64) fail("too many continue in a loop");
    loop_cont[i * 64 + loop_cont_n[i]] = code_len;
    loop_cont_n[i] = loop_cont_n[i] + 1;
    emit1(0xe9); emit4(0);
    return 0;
}
int loop_patch_break(int target) {
    int i = nloop - 1;
    int j = 0;
    while (j < loop_brk_n[i]) {
        int p = loop_brk[i * 64 + j];
        save32(p + 1, target - (p + 5));
        j = j + 1;
    }
    return 0;
}
int loop_patch_continue(int target) {
    int i = nloop - 1;
    int j = 0;
    while (j < loop_cont_n[i]) {
        int p = loop_cont[i * 64 + j];
        save32(p + 1, target - (p + 5));
        j = j + 1;
    }
    return 0;
}

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
    /* FIX-A（minicc.c 同步）：十六进制数组长度（旧实现十进制环 → 0x10=7210）；非数字一律拒绝 */
    int n = 0; int i; int d; int v;
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        if (tok[2] == 0) fail("bad number");
        i = 2;
        while (tok[i]) {
            d = tok[i]; v = 0;
            if (d >= '0' && d <= '9') v = d - '0';
            else if (d >= 'a' && d <= 'f') v = d - 'a' + 10;
            else if (d >= 'A' && d <= 'F') v = d - 'A' + 10;
            else fail("bad number");
            if (n > 0x0fffffff) fail("array size overflow");
            n = n * 16 + v;
            i = i + 1;
        }
    } else {
        i = 0;
        while (tok[i]) {
            if (tok[i] < '0' || tok[i] > '9') fail("bad number");
            if (n > 0x0fffffff) fail("array size overflow");
            n = n * 10 + (tok[i] - '0');
            i = i + 1;
        }
    }
    if (n < 1) fail("array size must be positive");
    next_tok();
    expect_s("]");
    *len = n;
    return 1;
}

/* FIX-B（minicc.c 同步）：数组字节数 int 乘法可溢出为 0/负值 → 相邻全局踩踏 / frame 守卫绕过。
 * 唯一入口做上限校验（宁拒绝勿产出坏码）。 */
int LOCAL_BYTES_MAX = 4096;
int GLOBAL_BYTES_MAX = 16777216;   /* 16MB（自举版无前处理，全局初始化须字面量；勿写 16*1024*1024） */
int bytes_of(int ty, int bty, int len, int limit) {
    int esz;
    if (ty != TY_ARRAY) return 4;
    if (bty == TY_INT) esz = 4; else esz = 1;
    if (len > limit / esz) fail("array too big");
    return len * esz;
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
            /* MC-08#1：调用点类型 = 函数返回类型（旧实现恒 TY_INT，char 返回被抹平） */
            nty[n] = sty[nval[n]];
            if (sty[nval[n]] == TY_PTR) nbty[n] = sbty[nval[n]]; else nbty[n] = 0;
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
            /* FIX-G（同步）：实参/形参个数一致性。已定义→直接比对；未定义→记录/比对，留待定义处核对。 */
            int fidx = nval[n];
            if (sdef[fidx] != 0) {
                if (nnargs[n] != snargs[fidx]) fail("arg count mismatch");
            } else if (snargs[fidx] >= 0) {
                if (nnargs[n] != snargs[fidx]) fail("arg count mismatch");
            } else {
                snargs[fidx] = nnargs[n];
            }
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

int unary_inner() {
    if (accept_s("++")) {               /* V3b：前缀 ++lv → 语法糖 lv = lv+1（值=新值） */
        return prefix_incdec(unary(), ND_ADD);
    }
    if (accept_s("--")) {               /* V3b：前缀 --lv → 语法糖 lv = lv-1 */
        return prefix_incdec(unary(), ND_SUB);
    }
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
    return postfix();
}

/* V3b：后缀 ++/-- 构造 ND_POST_INC / ND_POST_DEC（表达式值为旧值，见 gen_inner）。
 * 左值校验与赋值族一致（contract：只作用于可寻址左值）。 */
int post_incdec(int operand, int kind) {
    if (nkind[operand] != ND_VAR && nkind[operand] != ND_DEREF && nkind[operand] != ND_INDEX)
        fail("increment/decrement of non-lvalue");
    int n = node_new(kind);
    nl[n] = operand;
    nty[n] = nty[operand];
    return n;
}

/* V3b：后缀表达式 = primary 后接任意 ++/--（优先级高于一元；函数调用已由 primary 消化） */
int postfix() {
    int n = primary();
    int more = 1;
    while (more) {
        if (accept_s("++")) n = post_incdec(n, ND_POST_INC);
        else if (accept_s("--")) n = post_incdec(n, ND_POST_DEC);
        /* #165：指针下标糖 `p[i]` ≡ `*(p+i)`——脱糖复用 bin 的指针缩放与 ND_DEREF 取宽，
         * 零新增 codegen。数组下标（TY_ARRAY）已由 primary 消化，此处只接指针；非指针拒。
         * bin 定义在本函数之后 → 隐式声明（返回 int，与定义一致）。 */
        else if (accept_s("[")) {
            if (nty[n] != TY_PTR) fail("subscript of non-pointer");
            int idx = expr();
            if (nty[idx] == TY_PTR || nty[idx] == TY_ARRAY) fail("index must be integer");
            expect_s("]");
            int d = node_new(ND_DEREF);
            nl[d] = bin(n, idx, ND_ADD);
            nty[d] = nbty[n];
            n = d;
        }
        else more = 0;
    }
    return n;
}

/* V3b：前缀 ++/-- → 语法糖 `++lv = lv = lv±1`。ND_ASSIGN 求值后值留在 eax（新值），
 * 恰为前缀表达式值。`lv±1` 用 bin(lv, 1, ADD/SUB)。 */
int prefix_incdec(int operand, int ck) {
    int one = node_new(ND_NUM);
    nval[one] = 1;
    nty[one] = TY_INT;
    return mk_assign(operand, bin(operand, one, ck));
}

/* MC-09 守卫 wrapper：一元链（-、!、~、*、&）每重嵌套 +1，与 expr 共享 edepth */
int unary() {
    if (edepth >= EXPR_DEPTH_MAX) fail("expression nesting too deep");
    edepth = edepth + 1;
    int r = unary_inner();
    edepth = edepth - 1;
    return r;
}

int bin(int l, int r, int kind) {
    int n = node_new(kind);
    nl[n] = l; nr[n] = r;
    if ((kind == ND_ADD || kind == ND_SUB) &&
        (nty[l] == TY_PTR || nty[r] == TY_PTR)) {
        /* 审计 MC-08（结果卡/D1）：p+p 指针相加静默接受（C 禁止）——双指针加法宁拒不坑 */
        if (kind == ND_ADD && nty[l] == TY_PTR && nty[r] == TY_PTR)
            fail("invalid operands: pointer + pointer");
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

/* V3b：复合赋值运算符（+= -= *= /= %=）→ 对应二元节点 kind；否则 -1。
 * 仅在当前 token 是这些两字符运算符时命中（字面量 token 不参与，与 accept_s 同一禁止面）。 */
int compound_op() {
    if (tok_is_word || tok_is_num || tok_is_str || tok_is_char) return -1;
    if (tok[0] == '+' && tok[1] == '=') return ND_ADD;
    if (tok[0] == '-' && tok[1] == '=') return ND_SUB;
    if (tok[0] == '*' && tok[1] == '=') return ND_MUL;
    if (tok[0] == '/' && tok[1] == '=') return ND_DIV;
    if (tok[0] == '%' && tok[1] == '=') return ND_MOD;
    return -1;
}

/* V3b：构造 ND_ASSIGN 并统一做左值/类型静态检查（`=`、复合赋值、前缀 ++/-- 共用） */
int mk_assign(int lv, int rhs) {
    int a = node_new(ND_ASSIGN);
    nl[a] = lv; nr[a] = rhs;
    if (nkind[lv] != ND_VAR && nkind[lv] != ND_DEREF && nkind[lv] != ND_INDEX)
        fail("assign to non-lvalue");
    if (type_eq(nty[lv], nbty[lv], nty[rhs], nbty[rhs]) == 0)
        fail("type mismatch in assignment");
    return a;
}

int expr_inner() {
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
    /* V3b：复合赋值 lv op= rhs → 语法糖改写为 lv = (lv op rhs)。右操作数右结合递归。 */
    int ck = compound_op();
    if (ck >= 0) {
        next_tok();                     /* 消费复合赋运算符 */
        int rhs = expr();               /* 右结合 */
        int op = bin(n, rhs, ck);       /* lv op rhs（含指针/取模语义；bin 参数序 (l,r,kind)） */
        return mk_assign(n, op);
    }
    return n;
}

/* MC-09 守卫 wrapper：括号/赋值右结合每重嵌套 +1，>EXPR_DEPTH_MAX 受控报错（与 unary 共享 edepth） */
int expr() {
    if (edepth >= EXPR_DEPTH_MAX) fail("expression nesting too deep");
    edepth = edepth + 1;
    int r = expr_inner();
    edepth = edepth - 1;
    return r;
}

int block_stmt_inner() {
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

/* MC-09 守卫 wrapper：语句块嵌套（{...} 内含 {...}）每层 +1，>STMT_DEPTH_MAX 受控报错 */
int block_stmt() {
    if (sdepth >= STMT_DEPTH_MAX) fail("statement nesting too deep");
    sdepth = sdepth + 1;
    int r = block_stmt_inner();
    sdepth = sdepth - 1;
    return r;
}

int stmt() {
    int n;
    if (is_sym_s("{")) {
        next_tok();
        return block_stmt();
    }
    if (accept_s(";")) {
        /* C 空语句：合成空 ND_BLOCK（与 host 同构；作用域纯 parse 期，无副作用） */
        n = node_new(ND_BLOCK);
        na[n] = 0;
        return n;
    }
    if (peek_s("int") || peek_s("char")) {
        /* 局部声明子句表（`int a,b,*c[3];`——与 host 严格同构，§7.3 双编译器契约） */
        int dty, dbty, dspec, dcnt, dhead, dtail2, dmore, dnoff, dsize;
        dty = decl_type(&dbty);
        if (dty == TY_PTR) dspec = dbty; else dspec = dty;   /* 自举语料禁 ternary（minicc 子集无 ?:） */
        dcnt = 0; dhead = 0; dtail2 = 0;
        while (1) {
            dcnt = dcnt + 1;
            n = node_new(ND_DECL);
            nlen[n] = 0;
            if (dcnt == 1) { nty[n] = dty; nbty[n] = dbty; }
            else if (accept_s("*") != 0) { nty[n] = TY_PTR; nbty[n] = dspec; }
            else { nty[n] = dspec; nbty[n] = 0; }
            if (tok_is_word == 0) fail("expected identifier");
            dnoff = stradd(&tok[0]);
            next_tok();
            if (array_suffix(nty[n], &nlen[n])) { nbty[n] = nty[n]; nty[n] = TY_ARRAY; }
            if (nty[n] == TY_ARRAY && peek_s("=")) fail("array init not supported");
            dsize = bytes_of(nty[n], nbty[n], nlen[n], LOCAL_BYTES_MAX);  /* FIX-B */
            cur_frame = cur_frame + dsize;
            if (cur_frame > 4096) fail("frame too big");
            nval[n] = cur_frame;
            sym_add(dnoff, K_LOCAL, nty[n], nbty[n], nlen[n], nval[n]);
            if (accept_s("=")) {
                nl[n] = expr();
                if (type_eq(nty[n], nbty[n], nty[nl[n]], nbty[nl[n]]) == 0)
                    fail("type mismatch in initialization");
            }
            if (dhead == 0) dhead = n; else nnext[dtail2] = n;
            dtail2 = n;
            dmore = (accept_s(",") != 0);
            if (dmore == 0) { expect_s(";"); break; }
        }
        if (dcnt == 1) return dhead;
        n = node_new(ND_BLOCK);          /* 多子句：子链包进合成块（消费方语义不变） */
        na[n] = dhead;
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
    /* V3b：do <body> while(<expr>); —— post-test 循环 */
    if (accept_s("do")) {
        n = node_new(ND_DO);
        nb[n] = stmt();             /* body（先执行一次） */
        expect_s("while");
        expect_s("(");
        nl[n] = expr();             /* 条件（body 后求值） */
        expect_s(")");
        expect_s(";");
        return n;
    }
    if (accept_s("break")) { n = node_new(ND_BREAK); expect_s(";"); return n; }
    if (accept_s("continue")) { n = node_new(ND_CONTINUE); expect_s(";"); return n; }
    n = node_new(ND_EXPR_STMT);
    nl[n] = expr();
    expect_s(";");
    return n;
}

int funcs; int funcs_tail; int gvars; int gvars_tail;

/* MC-08#2 落尾可达性（与 host minicc.c 同步近似）：BLOCK 看最后一条、IF 双分支皆 return 才成立；
 * while(字面量非零)/for(;;) 近似无限循环不落到末尾（mul/add 等 `while(1){...else return}` 不报假告警）；
 * 其余视为可落到末尾 → 命中 1 不回告警，未命中 0 则函数可能 control reaches end（残留 eax）。 */
int ends_in_ret(int s) {
    if (s == 0) return 0;
    if (nkind[s] == ND_RET) return 1;
    if (nkind[s] == ND_BLOCK) {
        int last = na[s];
        if (last == 0) return 0;
        while (nnext[last] != 0) last = nnext[last];
        return ends_in_ret(last);
    }
    if (nkind[s] == ND_IF) {
        if (nb[s] == 0) return 0;
        if (ends_in_ret(nr[s]) != 0 && ends_in_ret(nb[s]) != 0) return 1;
        return 0;
    }
    if (nkind[s] == ND_WHILE) {
        int c = nl[s];              /* while(l) body=r */
        if (c != 0 && nkind[c] == ND_NUM && nval[c] != 0) return 1;
        return 0;
    }
    if (nkind[s] == ND_FOR) {
        int c = nr[s];              /* for(init;r;step) body=b，条件空 = for(;;) */
        if (c == 0) return 1;
        if (nkind[c] == ND_NUM && nval[c] != 0) return 1;
        return 0;
    }
    return 0;
}

int parse_program() {
    while (1) {
        if (tok[0] == 0) return 0;
        if (peek_s("int") == 0 && peek_s("char") == 0) fail("expected type");
        int ty = decl_type(&bty_top);
        if (tok_is_word == 0) fail("expected identifier");
        int noff = stradd(&tok[0]);
        next_tok();
        len_top = 0;
        int spec_ty;
        if (ty == TY_PTR) spec_ty = bty_top; else spec_ty = ty;   /* 子句表共享基底；禁 ternary 同因 */
        if (array_suffix(ty, &len_top)) { bty_top = ty; ty = TY_ARRAY; }
        if (accept_s("(")) {
            int si = sym_find(noff);
            if (si >= 0) {
                if (skind[si] != K_FUNC || sdef[si] != 0) fail("redefined");
                /* MC-08#1：先前隐式声明带 TY_INT，真定义补写真实返回类型 */
                sty[si] = ty;
                if (ty == TY_PTR) sbty[si] = bty_top; else sbty[si] = 0;
                sdef[si] = 1;   /* MC-08 末角（同步）：函数已定义（parse 期标记；旧 sval>=0 恒假） */
            } else {
                /* MC-08#1：返回类型不再抹平为 TY_INT（decl_type 返回值被丢弃，
                 * 使 `char cf()` 在调用点当 int → "int→ptr 放宽"错接住 `int* p=cf()`） */
                si = sym_add(noff, K_FUNC, ty, bty_top, 0, -1);
                sdef[si] = 1;   /* MC-08 末角（同步） */
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
            /* FIX-G（同步）：定义处交叉核对先前同名调用记录的实参个数 */
            if (snargs[si] >= 0 && snargs[si] != cur_nargs)
                fail("arg count mismatch");
            snargs[si] = cur_nargs;      /* 固化形参个数 */
            na[fn] = params;
            /* MC-08#3：入口 stub 是 `call main` 不带参，main 带形参时 argc 读垃圾/0（与 gcc 参考差 1 位），宁拒不坑 */
            if (seq(noff, "main") != 0 && cur_nargs > 0) fail("main takes no arguments");
            if (accept_s("{") == 0) fail("expected function body");
            nb[fn] = block_stmt();
            /* MC-08#2：落尾可达 return 检查（非致命告警，不中断编译；与 -Wreturn-type 精神一致） */
            if (ends_in_ret(nb[fn]) == 0) {
                sys_print("minicc: warning: function '");
                sys_print(&strtab[nival[fn]]);
                sys_print("' control reaches end of function without return (returns residual eax)\n");
            }
            nnlocals[fn] = cur_frame;
            nsym = func_scope;
            if (funcs == 0) funcs = fn; else nnext[funcs_tail] = fn;
            funcs_tail = fn;
        } else {
            /* 全局声明子句表（`int a,b=2,*c;`——host 同构；C89 顶部预声明，guest 编得动） */
            int gty, gbty, glen, gcurr, gspec, gsic, gcont;
            gty = ty; gbty = bty_top; glen = len_top; gcurr = noff;
            gspec = spec_ty; gcont = 1;
            while (gcont) {
            int g; int si;
            if (sym_find(gcurr) >= 0) fail("redefined");
            g = node_new(ND_GVAR);
            nival[g] = gcurr;
            nty[g] = gty; nbty[g] = gbty; nlen[g] = glen;
            si = sym_add(gcurr, K_GLOBAL, gty, gbty, glen, 0);
            nval[g] = si; gsic = si;
            if (accept_s("=")) {
                if (gty == TY_ARRAY) fail("array init not supported");
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
            if (gvars == 0) gvars = g; else nnext[gvars_tail] = g;
            gvars_tail = g;
            if (accept_s(",") != 0) {
                if (accept_s("*") != 0) { gty = TY_PTR; gbty = gspec; }
                else                    { gty = gspec;  gbty = 0;    }
                if (accept_s("(")) fail("unsupported: function declarator in list");
                if (tok_is_word == 0) fail("expected identifier");
                gcurr = stradd(&tok[0]);
                next_tok();
                glen = 0;
                if (array_suffix(gty, &glen)) { gbty = gty; gty = TY_ARRAY; }
            } else {
                expect_s(";");
                gcont = 0;
            }
            }
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

int gen_inner(int n) {
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
    if (nkind[n] == ND_POST_INC || nkind[n] == ND_POST_DEC) {
        /* V3b（与 minicc.c 同步）：后缀 ++/-- 表达式值为旧值。ebx 暂存左值地址；
         * 仅复用现有 store/load 指令，无新 emit 原语。 */
        int width = 4;
        if (nty[nl[n]] == TY_CHAR) width = 1;
        gen_addr(nl[n]);           /* eax = 左值地址 */
        emit_op("\x89\xc3");       /* mov %eax,%ebx */
        if (width == 1) emit_op("\x0f\xb6\x03");   /* movzbl (%ebx),%eax */
        else emit_op("\x8b\x03");                  /* mov (%ebx),%eax */
        emit1(0x50);               /* push 旧值 */
        if (nkind[n] == ND_POST_INC) emit_op("\x83\xc0\x01");  /* add $1,%eax */
        else emit_op("\x83\xe8\x01");                          /* sub $1,%eax */
        if (width == 1) emit_op("\x88\x03");       /* mov %al,(%ebx) */
        else emit_op("\x89\x03");                  /* mov %eax,(%ebx) */
        emit_op("\x58");           /* pop %eax：旧值 */
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

/* MC-09 守卫 wrapper：codegen 随 AST 深度递归（深括号或长加法链），>GEN_DEPTH_MAX 受控报错 */
int gen(int n) {
    if (gdepth >= GEN_DEPTH_MAX) fail("expression nesting too deep");
    gdepth = gdepth + 1;
    gen_inner(n);
    gdepth = gdepth - 1;
    return 0;
}

int gen_stmt_inner(int n) {
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
        int top = code_len;                 /* continue 目标 = 条件测试 */
        int en = new_lab();
        loop_enter();
        gen(nl[n]); emit_test(); emit_cond(0x84, en);
        gen_stmt(nr[n]);
        emit_jmp_to(top);
        loop_patch_break(code_len);         /* break 出口 = 循环末尾 */
        loop_patch_continue(top);           /* continue → 回测条件 */
        loop_leave();
        patch_lab(en, code_len);
        return 0;
    }
    if (nkind[n] == ND_FOR) {
        if (nl[n] != 0) gen(nl[n]);
        int top = code_len;                 /* 条件测试起点 */
        /* FIX-C1（minicc.c 同步，MC-01）：条件为空不申请 en（en=-1）→ 未 emit 标签不回填，
         * 否则 lpos 保持 0，finish 按 pos+2 把跳转立即数写进 ELF 头，产物被内核拒载 */
        int en = -1;
        loop_enter();
        if (nr[n] != 0) { en = new_lab(); gen(nr[n]); emit_test(); emit_cond(0x84, en); }
        gen_stmt(nb[n]);
        int cont_pt = code_len;             /* continue 目标 = step 起点（无 step 则退到 jmp-to-cond） */
        if (na[n] != 0) gen(na[n]);
        emit_jmp_to(top);
        loop_patch_break(code_len);
        loop_patch_continue(cont_pt);
        loop_leave();
        if (en >= 0) patch_lab(en, code_len);
        return 0;
    }
    if (nkind[n] == ND_DO) {
        int top = code_len;                 /* body 起点（先执行一次） */
        loop_enter();
        gen_stmt(nb[n]);
        int cont_pt = code_len;             /* continue 目标 = body 之后的条件求值 */
        gen(nl[n]); emit_test();
        emit_cond_direct(0x85, top);        /* jnz 回 body（条件真则重跑，向后直接跳） */
        loop_patch_break(code_len);         /* break 出口 = 循环末尾 */
        loop_patch_continue(cont_pt);
        loop_leave();
        return 0;                           /* 条件假 → 自然落出循环 */
    }
    if (nkind[n] == ND_BREAK) {
        if (nloop == 0) fail("break outside loop");
        loop_brk_add();
        return 0;
    }
    if (nkind[n] == ND_CONTINUE) {
        if (nloop == 0) fail("continue outside loop");
        loop_cont_add();
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

/* MC-09 守卫 wrapper：gen_stmt 随语句嵌套递归，与 gen 共享 gdepth */
int gen_stmt(int n) {
    if (gdepth >= GEN_DEPTH_MAX) fail("expression nesting too deep");
    gdepth = gdepth + 1;
    gen_stmt_inner(n);
    gdepth = gdepth - 1;
    return 0;
}

int gen_global(int n) {
    int si = nval[n];
    int pos = code_len;
    int size = bytes_of(nty[n], nbty[n], nlen[n], GLOBAL_BYTES_MAX);  /* FIX-B */
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
        /* FIX-C2（minicc.c 同步，MC-01 类）：lpos < 95 ⇔ 标签分配未发射 → 内部缺陷，显式失败 */
        if (lpos[lab] < 95) fail("internal: label not emitted");
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
        /* IN_LIMIT 而非 IN_CAP：须留一个读块（4096）余量，否则"in_len 刚过上限一点点"
         * 时那次 4096 读会越界写缓冲（旧码 60000 对 65536 正是这么留的，此处把关系写明）。 */
        if (in_len >= IN_LIMIT) fail("input too big");
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
    /* 输出缓冲容量（不是产物长度）：emit1 不扩容，故它同时是"产物硬上限"。
     * 节点池动态化后产物只剩代码与常量（≈154 KB），500000 留了 3 倍余量。 */
    code_cap = 500000;
    code = xmalloc(code_cap);
    in = xmalloc(IN_CAP);
    /* #172：节点池 15 个并行数组的运行时分配（见文件上方「内存池」段）。
     * char 字段 1 B/节点，int 字段 4 B/节点；句柄索引 0..NMAX-1，与 node_new 的守卫配套。
     * 放在解析之前——任何 node_new/数组访问都在这之后发生。 */
    nkind = xmalloc(NMAX);        nty = xmalloc(NMAX);      nbty = xmalloc(NMAX);
    nvkind = xmalloc(NMAX);       nnargs = xmalloc(NMAX);   nnlocals = xmalloc(NMAX);
    nlen = xmalloc(NMAX * 4);     nl = xmalloc(NMAX * 4);   nr = xmalloc(NMAX * 4);
    na = xmalloc(NMAX * 4);       nb = xmalloc(NMAX * 4);   nnext = xmalloc(NMAX * 4);
    nval = xmalloc(NMAX * 4);     nvslot = xmalloc(NMAX * 4); nival = xmalloc(NMAX * 4);
    strtab = xmalloc(STRTAB_CAP);   /* 名字池（④；同样必须在解析前就位） */
    code_len = 0;
    nsym = 0; npatch = 0; nlab = 0; nstr = 0; nn = 0; nstrpool = 0;
    funcs = 0; funcs_tail = 0; gvars = 0; gvars_tail = 0;
    if (open_input("/minicc.c") != 0) {
        sys_print("minicc: input open fail\n");
        return 1;
    }
    src_pos = 0; src_len = in_len;
    { int i = 0; while (i < src_len) {      /* FIX-F（同步）：拒绝源码中的原始 NUL 字节 */
        if ((*(in + i) & 255) == 0) fail("NUL byte in source"); i = i + 1; } }
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
