/* mini-os/v2-c-kernel/tools/minicc/minicc.c
 * minicc —— mini-os 自研小型 C 编译器（当前子集/能力矩阵见 docs/design/minicc-design.md §6）。
 *
 * 版权与许可：
 *   Copyright (C) 2026 mini-os authors
 *   SPDX-License-Identifier: MIT
 *   本项目（mini-os）整体为 MIT 许可。cc500 为 GPL 工具链组件；minicc 是
 *   全新 MIT 独立实现（非 cc500 派生），cc500 长期保留为教学对照（不退役）。
 *
 * 架构与特性矩阵（**唯一事实来源**，勿在此头注释复制枚举——一改两处必漂）：
 *   - 语言子集/能力矩阵/版本演进 → docs/design/minicc-design.md §6、§11
 *   - 差分对拍网（能力集 CAPS）→ tools/minicc/diffsynth/gen.c
 *   - 单遍→AST 等结构决策历史 → 同上 doc，不在此重述
 *
 * 头注释只声明"为何/不变式"，不随子集逐版改写：
 *   - 子集**契约式锁定**：拒绝必为编译期静态报错，绝不产出坏码；
 *   - 三层验证：宿主(秒级) + guest(真机语义) + 自举不动点(P1==P2)；
 *   - 新增语法须"三同步"（见下方【开发规约】），并同步 §6 特性矩阵。
 *
 * 当前能力速览（逐特性测试矩阵请读 §6）：int/char/数组/指针/全局、
 * 字符串与字符、位运算（& | ^ << >> ~）、if/while/for/do-while/return、
 * 循环控制（break/continue）、函数/递归、
 * 复合赋值（+= -= *= /= %=）、前/后缀 ++/--（V3b 语法糖）、
 * 隐式声明并调用 syscall3(n,a,b,c) stub。拒绝即编译期静态报错（契约式锁定）。
 *
 * 【开发规约 · 新增语法特性须"三同步"】⚠ dev 常看这里：
 *   给 minicc 加新语法时，一并：
 *     ① 在 tests/test_minicc.sh 加直接用例（minicc 特性不发散出回归网的基座）；
 *     ② 决定是否纳入差分对拍：tools/minicc/diffsynth/gen.c 的 CAPS_* 加对应 F_* flag
 *        + 配套生成逻辑（生成器是消费者，加特性 = 表加一行 + 一个生成函数）；
 *     ③ 故意不入差分网时，在 gen.c/本文档写一行注释说明原因。
 *   漏 ② 会让新特性游离在差分网外成"静默盲区"（CI 照样绿）——这正是 ops 点的软肋。
 *
 * 代码生成（x86 32 位，直出 ELF32）：
 *   - 标准 ebp 栈帧；表达式值在 eax；二元运算 push 左操作数。
 *   - 调用约定（自洽）：参数从左到右求值逐个 push；第 i 个形参位于 [ebp+8+4*(n-1-i)]。
 *   - ELF 布局：0x00 头+程序头，0x54 入口 stub
 *       call main ; mov %eax,%ebx ; xor %eax,%eax ; int $0x80
 *     （main 返回值作 sys_exit 退出码：mini-os ABI eax=号 ebx=a 返回 ebx）。
 *   - char 值无符号（movzbl 读 / mov %al 写）；指针算术按基类型缩放（int×4，char×1）；
 *     局部帧按**纯字节偏移**分配（数组紧凑 len×元素尺寸，标量不保证 4 对齐，x86 未对齐访问允许）。
 *
 * 运行环境：guest 由 minicc_crt.c 提供 syscall3（int $0x80）；host 由
 *   tools/minicc/host_crt.c 提供（Linux 文件模拟）。仅依赖 syscall3(0/1/13/14/15/16/17/19/35)。
 */

#include <stdint.h>
#include <stddef.h>

#if defined(MINICC_MOCK)
/* 仅宿主白盒 Mock 单测（tests/test_minicc_mock.c 以 -DMINICC_MOCK 编入）：
 * fail 本为 noreturn（sys_exit），"返回式回调"会让错误路径调用方继续空转拼出越界态；
 * 故 Mock 注入改为 setjmp/longjmp 就地捕获——fail 走 longjmp 回测试现场，minicc 停摆。
 * 正式（host_crt/minicc_crt/guest/自举）构建不定义 MINICC_MOCK，此项不编入 → 布局零变化。 */
#include <setjmp.h>
#endif

/* ============ 编译器上下文 CC：把分散的编译期可变全局收拢成单例（任务 5） ============
 * 收敛 33 个文件级全局到 cc_t（结构体即"注入点"，给 Mock 单测留缝）。
 * 第 1 阶段用 #define 别名让既有调用点零改动解析进 cc.field（P1==P2 零行为变化）；
 * 后续阶段逐类摘除 #define、把 cc 显式穿针（见 docs/design/minicc-v3-后续任务.md 任务5）。
 * 依赖类型（Sym/Patch/Lab/Node）与容量宏前置到本处，使别名先于首个使用点（fail() 用到 src）。 */

#define SYM_MAX 512
#define PATCH_MAX 4096              /* V3：自举需 ~2K 引用（函数调用+全局地址），1024 溢出 */
#define LAB_MAX 4096
#define TOK_MAX 256                 /* V2c：字符串字面量上限 254 字节（含解码） */

/* MC-09 递归深度守卫 × guest 用户栈仅 28KB（src/mm/mem.h USER_STACK_SLOT 32KB - GURD 4KB）：
 * 合法/畸形深表达式会让解析与 codegen 的递归打爆编译器自身栈（SIGSEGV，无编译期诊断）。
 * 限幅按 28KB 预算反推（repro.sh MC-09：括号≥45 即崩、加法链≥1000 即崩）留出安全余量：
 *   - EXPR_DEPTH_MAX 32  表达式/操作数嵌套（括号、一元链：primary 括号分支 + unary 入口）
 *   - STMT_DEPTH_MAX 128 语句/块嵌套（block_stmt 入口）
 *   - GEN_DEPTH_MAX  256 codegen AST 深度（深括号或长链，gen/gen_stmt 入口）
 * 超限一律 fail("expression nesting too deep") / fail("statement nesting too deep") 受控报错。 */
#define EXPR_DEPTH_MAX 32
#define STMT_DEPTH_MAX 128
#define GEN_DEPTH_MAX  256

typedef struct {
    char name[32];
    int kind;
    int ty;                     /* TY_INT / TY_CHAR / TY_PTR / TY_ARRAY */
    int bty;                    /* 指针基类型 / 数组元素类型（非指针数组时无意义） */
    int len;                    /* V2d：数组元素个数（非数组=0） */
    int val;                    /* FUNC: 代码偏移(未定义=-1)；GLOBAL: 数据偏移；
                                   LOCAL: 帧字节偏移；ARG: 参数序号 */
    int nargs;                  /* FUNC: 形参个数（未定义/未知=-1）——MC-04 实参/形参个数校验收敛用 */
} Sym;

typedef struct { char name[32]; int pos; int kind; } Patch;
typedef struct { int pos; int kind; int target; } Lab;

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

typedef struct {
    /* 输入 / 词法 */
    const unsigned char *src;  int src_len, src_pos;
    char tok[TOK_MAX]; int tok_is_word, tok_is_num, tok_is_str, tok_is_char, toklen;
    /* 输出码流 */
    unsigned char *code; int code_len, code_cap;
    /* 符号表 */
    Sym syms[SYM_MAX]; int nsym;
    /* 补丁 / 标签 */
    Patch patches[PATCH_MAX]; int npatch;
    Lab labs[LAB_MAX]; int nlab;
    /* codegen 译状态 */
    int cur_nargs, frame_patch, cur_frame, bty_top, len_top;
    /* 循环帧栈（ND_DO/WHILE/FOR 的 break/continue 未决跳转记录，循环收尾统一回填相对位移）。
     * 不用单 label 的 labs[]（其"每标签单补丁"限制无法承载一循环多个 break），
     * 改为每帧累积 E9 跳转的 code 位置，帧尾按目标批量 save32。 */
    int loop_brk[32][64]; int loop_brk_n[32];
    int loop_cont[32][64]; int loop_cont_n[32];
    int nloop;
    /* MC-09 递归深度计数器：解析（edepth/sdepth）与 codegen（gdepth）各自守卫，超限受控报错 */
    int edepth, sdepth, gdepth;
    /* 字符串池 */
    unsigned char *strpool; int nstrpool, strpool_cap, strpool_base;
    /* AST 链表 */
    Node *funcs; Node **funcs_tail; Node *gvars; Node **gvars_tail;
    /* 输入文件 */
    unsigned char *in_data; int in_len;
#if defined(MINICC_MOCK)
    /* Mock 测试注入（仅 -DMINICC_MOCK 的宿主白盒单测编译时存在；正式构建无此字段，布局零变化）：
     * 测试先 setjmp(cc.fail_jb) 并置 fail_jmp_on=1 → fail 改走 longjmp 回到测试现场，
     * 经 cc.fail_msg 就地捕获错误消息而不 sys_exit 退进程（任务4/5 Mock 规划）。 */
    jmp_buf fail_jb;
    const char *fail_msg;
    int fail_jmp_on;
#endif
} CC;

static CC cc;

/* 收敛别名：既有调用点一律按旧名解析进 cc.field */
#define src       cc.src
#define src_len   cc.src_len
#define src_pos   cc.src_pos
#define tok       cc.tok
#define tok_is_word cc.tok_is_word
#define tok_is_num cc.tok_is_num
#define tok_is_str cc.tok_is_str
#define tok_is_char cc.tok_is_char
#define toklen    cc.toklen
#define code      cc.code
#define code_len  cc.code_len
#define code_cap  cc.code_cap
#define syms      cc.syms
#define nsym      cc.nsym
#define patches   cc.patches
#define npatch    cc.npatch
#define labs      cc.labs
#define nlab      cc.nlab
#define cur_nargs cc.cur_nargs
#define frame_patch cc.frame_patch
#define cur_frame cc.cur_frame
#define bty_top   cc.bty_top
#define len_top   cc.len_top
#define strpool   cc.strpool
#define nstrpool  cc.nstrpool
#define strpool_cap cc.strpool_cap
#define strpool_base cc.strpool_base
#define funcs     cc.funcs
#define funcs_tail cc.funcs_tail
#define gvars     cc.gvars
#define gvars_tail cc.gvars_tail
#define in_data   cc.in_data
#define in_len    cc.in_len

/* ================= 系统调用与运行时基础 ================= */

int syscall3(int n, int a, int b, int c);   /* 由 CRT 提供 */

static void sys_print(const char *s) {
    syscall3(1, (int)s, 0, 0);              /* SYS_PRINT */
}

const char *tokbuf_current(void);       /* 定义在词法章节 */
/* （src / src_len / src_pos 已收进顶部 CC 上下文，见文件头） */

/* 编译错误：打印上下文 token 后以 1 退出（host/guest 均由 sys_exit 兜底）。
 * Mock 注入：-DMINICC_MOCK 且测试置 cc.fail_jmp_on 时，改走 longjmp 回测试现场（就地捕获，
 * 不退进程；fail 本 noreturn，"返回式回调"会让错误路径调用方继续空转出越界态，故必须 longjmp）。 */
static void fail(const char *msg) {
#if defined(MINICC_MOCK)
    if (cc.fail_jmp_on) { cc.fail_msg = msg; longjmp(cc.fail_jb, 1); }
#endif
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

static void s_cpy(char *dst, const char *sp) {
    while (*sp) *dst++ = *sp++;
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
/* （code / code_len / code_cap 已收进顶部 CC 上下文，见文件头） */

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

enum { K_FUNC, K_GLOBAL, K_LOCAL, K_ARG };
enum { TY_INT, TY_CHAR, TY_PTR, TY_ARRAY };  /* V2c：char（无符号）；V2d：数组 */
/* （Sym 类型 / syms / nsym 已收进顶部 CC 上下文，见文件头"编译器上下文 CC"） */

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
    syms[nsym].nargs = kind == K_FUNC ? -1 : 0;   /* FUNC 形参个数未知；其余无意义 */
    return nsym++;
}

/* ================= 补丁（符号引用 / 控制流标签） ================= */

enum { P_CALL, P_ADDR };
/* （Patch 类型 / patches / npatch 已收进顶部 CC 上下文，见文件头） */
static void patch_add(const char *name, int pos, int kind) {
    if (npatch >= PATCH_MAX) fail("too many references");
    s_cpy(patches[npatch].name, name);
    patches[npatch].pos = pos;
    patches[npatch].kind = kind;
    npatch++;
}

enum { L_COND, L_JMP };
/* （Lab 类型 / labs / nlab 已收进顶部 CC 上下文，见文件头） */
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

/* 直接目标的条件跳转（0F <op> rel32，向后/绝对目标用，不走 labs 前向补丁）：
 * 指令长 6 字节，rel 相对"指令尾" target - (p+6)。 */
static void emit_cond_direct(int op, int target) {
    int p = code_len;
    emit1(0x0F); emit1(op); emit4(target - (p + 6));
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
    ND_POST_INC, ND_POST_DEC,  /* V3b：后缀 ++/--（需返回旧值，l=左值，见 gen()） */
    ND_EXPR_STMT, ND_BLOCK, ND_IF, ND_WHILE, ND_FOR, ND_RET,
    ND_DECL, ND_FUNC, ND_GVAR,
    ND_DO, ND_BREAK, ND_CONTINUE   /* 循环语句补齐：do-while / break / continue */
};
/* （Node 类型已收进顶部 CC 上下文，见文件头"编译器上下文 CC"） */

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
/* （cur_nargs / frame_patch 已收进顶部 CC 上下文，见文件头） */

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
/* （tok / tok_is_word 等 / toklen 已收进顶部 CC 上下文，见文件头） */

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
        /* FIX-D（外部审计 MC-02）：Sym.name/Node.name 均 `char name[32]`，写入用无界 s_cpy。
         * 词法允许最长 TOK_MAX-1=255 字符标识符 → 栈上 name[32] 越界（36-37 字符静默坏码、
         * ≥38 SIGSEGV）。此处在词法层提前拒绝，与 `char name[32]` 上限一致（宁拒不坑）。 */
        if (n >= 32) fail("identifier too long (max 31)");
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
            if (n == 2) fail("empty hex literal");   /* FIX-E（审计 MC-07）：0x 后无十六进制位，宁拒不坑 */
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
        /* FIX-E（审计 MC-07）：minicc 不支持八进制，`010` 若按十进制会被静默解成 10（C 语义应为 8）。
         * 拒绝前导 0 + 多位数字的八进制形态，宁拒不误导（`0` 单个合法；0x 前置分支不落到此）。 */
        if (tok[0] == '0' && tok[1]) fail("octal literals not supported");
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
        (c == '&' && d == '&') || (c == '|' && d == '|') ||
        (c == '+' && d == '=') || (c == '-' && d == '=') ||
        (c == '*' && d == '=') || (c == '/' && d == '=') ||
        (c == '%' && d == '=') || (c == '+' && d == '+') ||
        (c == '-' && d == '-')) {
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

/* V2d：标识符之后的数组后缀 [N]（N 为常量 ≥1，指针数组拒绝）；
 * 命中则置 *len 并返回 1，否则 *len=0 返回 0。 */
static int array_suffix(int ty, int *len) {
    *len = 0;
    if (!accept("[")) return 0;
    if (ty == TY_PTR) fail("unsupported: array of pointers");
    if (!tok_is_num) fail("array size must be a constant");
    /* FIX-A（外部审计 MC-05）：与 primary()/GVAR 同构的字面量解析。
     * 旧实现 `n = n*10 + (tok[i]-'0')` 按十进制环吃整个 token —— `int g[0x10]` 被当成 7210 个
     * 元素（把 'x'=0x78 当位算 40），与 BUG-039（GVAR hex 初值）同源。非数字字符一律拒绝。 */
    int n = 0;
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        if (!tok[2]) fail("bad number");
        for (int i = 2; tok[i]; i++) {
            int d = tok[i], v = 0;
            if (d >= '0' && d <= '9') v = d - '0';
            else if (d >= 'a' && d <= 'f') v = d - 'a' + 10;
            else if (d >= 'A' && d <= 'F') v = d - 'A' + 10;
            else fail("bad number");
            if (n > 0x0fffffff) fail("array size overflow");
            n = n * 16 + v;
        }
    } else {
        for (int i = 0; tok[i]; i++) {
            if (tok[i] < '0' || tok[i] > '9') fail("bad number");
            if (n > 0x0fffffff) fail("array size overflow");
            n = n * 10 + (tok[i] - '0');
        }
    }
    if (n < 1) fail("array size must be positive");
    next_tok();
    expect("]");
    *len = n;
    return 1;
}

/* FIX-B（外部审计 MC-03）：数组字节数 = len × 元素尺寸 的 int 乘法可溢出为 0/负值：
 *   0 → 相邻全局互相覆盖（int g[1073741824] + int h 时 g[0] 写坏 h，实测）；
 *   负 → 绕过 "cur_frame > 4096" 守卫（负数对其恒不成立），产出非法 lea/sub，产物一执行即崩。
 *   故在唯一入口做上限校验（宁拒绝勿产出坏码 = 设计文档原则 3）。 */
#define LOCAL_BYTES_MAX 4096        /* 与既有 "frame too big" 守卫同额度（guest 帧上限） */
#define GLOBAL_BYTES_MAX (16 * 1024 * 1024)   /* 全局数据段上限（自举版数据 ~340KB，宽裕） */
static int bytes_of(int ty, int bty, int len, int limit) {
    int esz;
    if (ty != TY_ARRAY) return 4;
    esz = (bty == TY_INT) ? 4 : 1;
    if (len > limit / esz) fail("array too big");
    return len * esz;
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
/* MC-09 深度守卫 wrappers 前向声明（被定义其前的递归体引用，不可缺失） */
static Node *unary(void);
static Node *primary(void);
static Node *block_stmt(void);
static void gen_stmt(Node *n);

/* 字符串池（V2c）：字面量解码字节线性累积，codegen 时整体 emit 进数据段（只读）。
 * strpool_base：池在 code 缓冲内的起始偏移（ELF 头 95 字节之后），ND_STR 寻址用。
 * （strpool / nstrpool / strpool_cap / strpool_base 已收进顶部 CC 上下文，见文件头） */

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
            /* MC-08#1：调用点类型 = 函数返回类型（旧实现恒 TY_INT，char 返回被抹平） */
            n->ty = syms[n->val].ty;
            n->bty = (syms[n->val].ty == TY_PTR) ? syms[n->val].bty : 0;
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
            /* FIX-G（审计 MC-04）：实参/形参个数一致性。被调函数已定义(形参已知)→直接比对；
             * 未定义→记录本次实参个数（首次记录 / 重复比对），留待定义处交叉核对。 */
            int fidx = si < 0 ? nsym - 1 : si;
            if (syms[fidx].val >= 0) {                       /* 已定义，形参个数已知 */
                if (n->nargs != syms[fidx].nargs) fail("arg count mismatch");
            } else if (syms[fidx].nargs >= 0) {              /* 已见同名调用 */
                if (n->nargs != syms[fidx].nargs) fail("arg count mismatch");
            } else {
                syms[fidx].nargs = n->nargs;                 /* 首次见，记录本次实参个数 */
            }
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

static Node *prefix_incdec(Node *operand, int ck);   /* V3b 前置声明（相互递归） */
static Node *postfix(void);

static Node *unary_inner(void) {
    if (accept("++")) {                 /* V3b：前缀 ++lv → 语法糖 lv = lv+1（值=新值） */
        return prefix_incdec(unary(), ND_ADD);
    }
    if (accept("--")) {                 /* V3b：前缀 --lv → 语法糖 lv = lv-1 */
        return prefix_incdec(unary(), ND_SUB);
    }
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
    return postfix();
}

/* MC-09 守卫 wrapper：一元链（-、!、~、*、&、++/-- 前缀）每重嵌套 +1，与 expr 共享 edepth */
static Node *unary(void) {
    if (cc.edepth++ >= EXPR_DEPTH_MAX) fail("expression nesting too deep");
    Node *r = unary_inner();
    cc.edepth--;
    return r;
}

/* V3b：后缀 ++/-- 构造 ND_POST_INC / ND_POST_DEC（表达式值为旧值，见 gen()）。
 * 左值校验与赋值族一致（contract：只作用于可寻址左值）。 */
static Node *post_incdec(Node *operand, int kind) {
    if (operand->kind != ND_VAR && operand->kind != ND_DEREF &&
        operand->kind != ND_INDEX)
        fail("increment/decrement of non-lvalue");
    Node *n = node_new(kind);
    n->l = operand;
    n->ty = operand->ty;
    return n;
}

/* V3b：后缀表达式 = primary 后接任意 ++/--（优先级高于一元；函数调用/下标已由 primary 消化） */
static Node *postfix(void) {
    Node *n = primary();
    for (;;) {
        if (accept("++"))      n = post_incdec(n, ND_POST_INC);
        else if (accept("--")) n = post_incdec(n, ND_POST_DEC);
        else return n;
    }
}

static Node *bin(Node *l, Node *r, int kind) {
    Node *n = node_new(kind);
    n->l = l;
    n->r = r;
    /* 指针算术只对 +/- 传播指针类型（bty 随指针侧）；其余运算结果为 int */
    if ((kind == ND_ADD || kind == ND_SUB) &&
        (l->ty == TY_PTR || r->ty == TY_PTR)) {
        /* 审计 MC-08（结果卡/D1）：p+p 指针相加静默接受（C 禁止）——双指针加法宁拒不坑；
         * 双指针相减在 C 属 ptrdiff，本子集不支持（codegen 已 `invalid pointer subtraction` 拒）。 */
        if (kind == ND_ADD && l->ty == TY_PTR && r->ty == TY_PTR)
            fail("invalid operands: pointer + pointer");
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

/* V3b：复合赋值运算符（+= -= *= /= %=）→ 对应二元节点 kind；否则 -1。
 * 仅在当前 token 是这些两字符运算符时命中（字面量 token 不参与，与 accept 同一禁止面）。 */
static int compound_op(void) {
    if (tok_is_word || tok_is_num || tok_is_str || tok_is_char) return -1;
    if (tok[0] == '+' && tok[1] == '=') return ND_ADD;
    if (tok[0] == '-' && tok[1] == '=') return ND_SUB;
    if (tok[0] == '*' && tok[1] == '=') return ND_MUL;
    if (tok[0] == '/' && tok[1] == '=') return ND_DIV;
    if (tok[0] == '%' && tok[1] == '=') return ND_MOD;
    return -1;
}

/* V3b：构造 ND_ASSIGN 并统一做左值/类型静态检查（`=`、复合赋值、前缀 ++/-- 共用） */
static Node *mk_assign(Node *lv, Node *rhs);

static Node *expr_inner(void) {
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
    /* V3b：复合赋值 lv op= rhs → 语法糖改写为 lv = (lv op rhs)。右操作数右结合递归。 */
    int ck = compound_op();
    if (ck >= 0) {
        next_tok();                     /* 消费复合赋运算符 */
        Node *rhs = expr();             /* 右结合 */
        Node *op = bin(n, rhs, ck);     /* lv op rhs（含指针/取模语义；bin 参数序 (l,r,kind)） */
        return mk_assign(n, op);
    }
    return n;
}

/* MC-09 守卫 wrapper：括号/赋值右结合/复合赋右操作数每重嵌套 +1，>EXPR_DEPTH_MAX 受控报错 */
static Node *expr(void) {
    if (cc.edepth++ >= EXPR_DEPTH_MAX) fail("expression nesting too deep");
    Node *r = expr_inner();
    cc.edepth--;
    return r;
}

/* V3b：构造 ND_ASSIGN 并统一做左值/类型静态检查（`=`、复合赋值、前缀 ++/-- 共用） */
static Node *mk_assign(Node *lv, Node *rhs) {
    Node *a = node_new(ND_ASSIGN);
    a->l = lv;
    a->r = rhs;
    if (lv->kind != ND_VAR && lv->kind != ND_DEREF && lv->kind != ND_INDEX)
        fail("assign to non-lvalue");
    if (!type_eq(lv->ty, lv->bty, rhs->ty, rhs->bty))
        fail("type mismatch in assignment");
    return a;
}

/* V3b：前缀 ++/-- → 语法糖 `++lv = lv = lv±1`。ND_ASSIGN 求值后值留在 eax（新值），
 * 恰为前缀表达式的值。`lv±1` 用 bin(lv, 1, ADD/SUB)，注意 bin 参数序为 (l,r,kind)。 */
static Node *prefix_incdec(Node *operand, int ck) {
    Node *one = node_new(ND_NUM);
    one->val = 1;               /* ND_NUM 数值存 val（gen 读 n->val），非 ival */
    one->ty = TY_INT;
    return mk_assign(operand, bin(operand, one, ck));
}

/* ---- 语句 ---- */
/* （cur_frame / bty_top / len_top 已收进顶部 CC 上下文，见文件头） */

static Node *stmt(void);

static Node *block_stmt_inner(void) {
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

/* MC-09 守卫 wrapper：语句块嵌套（{...} 内含 {...}）每层 +1，>STMT_DEPTH_MAX 受控报错 */
static Node *block_stmt(void) {
    if (cc.sdepth++ >= STMT_DEPTH_MAX) fail("statement nesting too deep");
    Node *r = block_stmt_inner();
    cc.sdepth--;
    return r;
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
        /* V2d：帧按纯字节偏移分配（数组紧凑 len×元素尺寸，标量不保证 4 对齐）；
         * FIX-B：走 bytes_of 唯一入口做溢出/上限校验（局部帧上限=4096） */
        int size = bytes_of(n->ty, n->bty, n->len, LOCAL_BYTES_MAX);
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
    if (accept("do")) {
        /* do <body> while (<expr>); —— post-test 循环 */
        n = node_new(ND_DO);
        n->b = stmt();              /* body（先执行一次） */
        expect("while");
        expect("(");
        n->l = expr();              /* 条件（body 后求值） */
        expect(")");
        expect(";");
        return n;
    }
    if (accept("break")) { n = node_new(ND_BREAK); expect(";"); return n; }
    if (accept("continue")) { n = node_new(ND_CONTINUE); expect(";"); return n; }
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
/* （funcs / funcs_tail / gvars / gvars_tail 已收进顶部 CC 上下文，见文件头） */

/* MC-08#2 落尾可达性：语句 s 是否保证以 return 收尾（不落到函数末尾）？
 * 近似（保守）：BLOCK 看最后一条、IF 双分支皆 return 才成立；while(字面量非零)/for(;;) 近似无限
 * 循环视为不落到末尾（mul/add 等 `while(1){...else return}` 惯用法不应报假告警）；其余视为可落到末尾。 */
static int ends_in_ret(Node *s) {
    if (!s) return 0;
    if (s->kind == ND_RET) return 1;
    if (s->kind == ND_BLOCK) {
        Node *last = s->a;
        if (!last) return 0;
        while (last->next) last = last->next;
        return ends_in_ret(last);
    }
    if (s->kind == ND_IF)
        return s->b && ends_in_ret(s->r) && ends_in_ret(s->b);
    if (s->kind == ND_WHILE) {
        Node *c = s->l;         /* while(l) body=r */
        if (c && c->kind == ND_NUM && c->val != 0) return 1;   /* 字面量非零条件 ≈ 永不落尾 */
        return 0;
    }
    if (s->kind == ND_FOR) {
        Node *c = s->r;         /* for(init;r;step) body=b，条件空 = for(;;) */
        if (c == NULL || (c->kind == ND_NUM && c->val != 0)) return 1;
        return 0;
    }
    return 0;
}

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
                /* MC-08#1：先前隐式声明（调用先于定义）带 TY_INT，真定义来了补写真实返回类型 */
                syms[si].ty = ty;
                syms[si].bty = (ty == TY_PTR) ? bty_top : 0;
            } else {
                /* MC-08#1：返回类型不再抹平为 TY_INT（旧实现 decl_type 的返回值被丢弃，
                 * 使 `char cf()` 在调用点被当 int → "int→ptr 放宽"错接住 `int* p=cf()`）。 */
                si = sym_add(name, K_FUNC, ty, bty_top, 0, -1);
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
            /* FIX-G（审计 MC-04）：定义处交叉核对先前同名调用记录的实参个数 */
            if (syms[si].nargs >= 0 && syms[si].nargs != cur_nargs)
                fail("arg count mismatch");
            syms[si].nargs = cur_nargs;    /* 固化形参个数 */
            fn->a = params;
            /* MC-08#3：入口 stub 是 `call main` 不带参，main 带形参时 argc 读到垃圾/0（与 gcc 参考差 1 位），
             * 用户无从知晓 —— 宁拒不坑（契约：main() 固定无参）。 */
            if (s_eq(fn->name, "main") && cur_nargs > 0)
                fail("main takes no arguments");
            if (!accept("{")) fail("expected function body");
            fn->b = block_stmt();
            /* MC-08#2：落尾可达 return 检查（非致命告警，不中断编译；与 -Wreturn-type 精神一致） */
            if (!ends_in_ret(fn->b)) {
                sys_print("minicc: warning: function '");
                sys_print(fn->name);
                sys_print("' control reaches end of function without return (returns residual eax)\n");
            }
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

static void gen_inner(Node *n) {
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
    case ND_POST_INC: /* fallthrough */
    case ND_POST_DEC: {
        /* 后缀 ++/--：表达式值为旧值。ebx 暂存左值地址；仅复用现有 store/load 指令，无新 emit 原语。 */
        int width = (n->l->ty == TY_CHAR) ? 1 : 4;
        gen_addr(n->l);            /* eax = 左值地址 */
        emit_op("\x89\xc3");       /* mov %eax,%ebx */
        if (width == 1) emit_op("\x0f\xb6\x03");   /* movzbl (%ebx),%eax */
        else emit_op("\x8b\x03");                  /* mov (%ebx),%eax */
        emit1(0x50);               /* push 旧值 */
        if (n->kind == ND_POST_INC) emit_op("\x83\xc0\x01");  /* add $1,%eax */
        else emit_op("\x83\xe8\x01");                          /* sub $1,%eax */
        if (width == 1) emit_op("\x88\x03");       /* mov %al,(%ebx) */
        else emit_op("\x89\x03");                  /* mov %eax,(%ebx) */
        emit_op("\x58");           /* pop %eax：旧值 */
        return;
    }
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

/* MC-09 守卫 wrapper：codegen 随 AST 深度递归（深括号或长加法链都汇聚于此），>GEN_DEPTH_MAX 受控报错 */
static void gen(Node *n) {
    if (cc.gdepth++ >= GEN_DEPTH_MAX) fail("expression nesting too deep");
    gen_inner(n);
    cc.gdepth--;
}

/* ================= 循环 break/continue 目标栈 =================
 * 进入循环时 loop_enter；body 内 break/continue 各自记下一个"正向 E9（占位 rel=0）"的
 * code 位置；循环收尾用 loop_patch_break(出口)/loop_patch_continue(续点) 批量回填。
 * 相对位移 = target - (jmp_pos + 5)（E9 rel32 长度为 5）。 */
static void loop_enter(void){ if(cc.nloop>=32) fail("loop nesting too deep"); cc.loop_brk_n[cc.nloop]=0; cc.loop_cont_n[cc.nloop]=0; cc.nloop++; }
static void loop_leave(void){ if(cc.nloop>0) cc.nloop--; }
static void loop_brk_add(void){ int i=cc.nloop-1; if(cc.loop_brk_n[i]>=64) fail("too many break in a loop"); cc.loop_brk[i][cc.loop_brk_n[i]++]=code_len; emit1(0xE9); emit4(0); }
static void loop_cont_add(void){ int i=cc.nloop-1; if(cc.loop_cont_n[i]>=64) fail("too many continue in a loop"); cc.loop_cont[i][cc.loop_cont_n[i]++]=code_len; emit1(0xE9); emit4(0); }
static void loop_patch_break(int target){ int i=cc.nloop-1; for(int j=0;j<cc.loop_brk_n[i];j++){ int p=cc.loop_brk[i][j]; save32(p+1, target-(p+5)); } }
static void loop_patch_continue(int target){ int i=cc.nloop-1; for(int j=0;j<cc.loop_cont_n[i];j++){ int p=cc.loop_cont[i][j]; save32(p+1, target-(p+5)); } }

static void gen_stmt_inner(Node *n) {
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
        int top = code_len;                 /* continue 目标 = 条件测试 */
        int en = new_lab();
        loop_enter();
        gen(n->l); emit_test(); emit_cond(0x84, en);
        gen_stmt(n->r);
        emit_jmp_to(top);
        loop_patch_break(code_len);         /* break 出口 = 循环末尾 */
        loop_patch_continue(top);           /* continue → 回测条件 */
        loop_leave();
        patch_lab(en, code_len);
        return;
    }
    case ND_FOR: {
        /* for(init;cond;step)body：l=init, r=cond(可空), a=step(可空), b=body
         * 空 init 时需额外跳过一个空语句（node_new 生 ND_EXPR_STMT(0) 亦可空跑） */
        if (n->l) gen(n->l);
        int top = code_len;                 /* 条件测试起点 */
        /* FIX-C1（外部审计 MC-01）：条件为空时不得申请 en（en=-1 哨兵）。
         * 旧实现无条件 new_lab()：未 emit 的标签 labs[en].pos 保持初值 0，
         * finish() 按 pos+2 回填 → save32(2,rel) 把跳转立即数写进 ELF 头(e_ident)，
         * 结果编译报 "compiled OK"、产物却被内核 elf_load 拒载。 */
        int en = -1;
        loop_enter();
        if (n->r) {
            en = new_lab();
            gen(n->r); emit_test(); emit_cond(0x84, en);
        }
        gen_stmt(n->b);
        int cont_pt = code_len;             /* continue 目标 = step 起点（无 step 则退到 jmp-to-cond） */
        if (n->a) gen(n->a);
        emit_jmp_to(top);
        loop_patch_break(code_len);
        loop_patch_continue(cont_pt);
        loop_leave();
        if (en >= 0) patch_lab(en, code_len);
        return;
    }
    case ND_DO: {
        int top = code_len;                 /* body 起点（先执行一次） */
        loop_enter();
        gen_stmt(n->b);
        int cont_pt = code_len;             /* continue 目标 = body 之后的条件求值 */
        gen(n->l); emit_test();
        emit_cond_direct(0x85, top);        /* jnz 回 body（条件真则重跑，向后直接跳） */
        loop_patch_break(code_len);         /* break 出口 = 循环末尾 */
        loop_patch_continue(cont_pt);
        loop_leave();
        return;                             /* 条件假 → 自然落出循环 */
    }
    case ND_BREAK:
        if (cc.nloop == 0) fail("break outside loop");
        loop_brk_add();
        return;
    case ND_CONTINUE:
        if (cc.nloop == 0) fail("continue outside loop");
        loop_cont_add();
        return;
    case ND_RET:
        if (n->l) gen(n->l);
        emit_epilogue();
        return;
    default:
        fail("internal: bad stmt node");
    }
}

/* MC-09 守卫 wrapper：gen_stmt 随语句嵌套（BLOCK/IF/循环）递归，与 gen 共享 gdepth */
static void gen_stmt(Node *n) {
    if (cc.gdepth++ >= GEN_DEPTH_MAX) fail("expression nesting too deep");
    gen_stmt_inner(n);
    cc.gdepth--;
}

/* 全局数据（ND_GVAR）：emit 变量字节（数组 0 填充；标量回写初值），登记符号偏移 */
static void gen_global(Node *n) {
    int si = n->val;
    int pos = code_len;
    int size = bytes_of(n->ty, n->bty, n->len, GLOBAL_BYTES_MAX);  /* FIX-B */
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
        /* FIX-C2（外部审计 MC-01 类）：labs[i].pos < 95（ELF 头 + 入口 stub 之前的字节
         * 永不可能是跳转指令）⇔ 有标签被分配却未发射（如空条件 for 未 emit_cond），
         * 属编译器内部缺陷。把整类"未发射标签"从静默写坏 ELF 头变为显式编译失败。 */
        if (labs[i].pos < 95) fail("internal: label not emitted");
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

/* （in_data / in_len 已收进顶部 CC 上下文，见文件头） */

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
    for (int i = 0; i < src_len; i++)    /* FIX-F（审计 MC-06）：拒绝源码中的原始 NUL 字节（`\0` 转义为 `\\0`，不受影响） */
        if (src[i] == 0) fail("NUL byte in source");
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
