/* mini-os/v2-c-kernel/tools/minicc/diffsynth/gen.c
 * 差分对拍生成器核心（mini-Csmith，MVP-A）。
 *
 * 形态：特性表驱动 + 能力集筛选（动态子集）——见 docs/design/minicc-v3-后续任务.md 任务4 设计约束。
 *   - 每个语法特性 = 一张"特性行"（生成函数 + 适用编译器集）。目标编译器能力集是数据(开关)，
 *     从全表筛出特性。加特性 = 表加一行 + 一个生成函数，核心/runner 不动。
 *   - 当前特性集限于 int 子集（const/局部变量/算术/比较/逻辑/if/while/for），与 minicc 支持子集对齐。
 *
 * 无 UB 三纪律（一票否决的灵魂设计）：
 *   1. 声明即初始化：局部 int 建立时喂已知常量窄值，无需"读前必写"状态机；
 *   2. 值域限幅：每变量/表达式维护保守区间 [lo,hi]，算术后重算并夹取到 [-2^28,2^28]，杜绝有符号溢出；
 *      除法/取模分母强制来自非零字面量（1..100），杜绝除零；
 *   3. 循环计数守卫：while/for 都带计数上限，保证确定性终止（否则差分会挂）。
 *   附加 gcc 哨兵由 harness 承担（每个用例先 gcc -O0 过一遍，过滤生成器 bug）。
 *
 * 用法：
 *   gen --seed <S> --count <N> --target <minicc|cc500> --vars <V> --stmts <S> --out <dir>
 *   gen --seed <S> --target <t>            # 打印单个程序到 stdout（供 harness 单例跑）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rng_state;
static unsigned rnd(void) {                /* xorshift32：可复现 per seed，无 libc rand 依赖 */
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}
static int rndi(int lo, int hi) {          /* [lo,hi] 闭区间 */
    if (hi <= lo) return lo;
    return lo + (int)(rnd() % (unsigned)(hi - lo + 1));
}
static int rbool(void) { return (int)(rnd() & 1u); }

/* ---- 特性表 + 能力集（动态子集核心） ----
 * minicc 支持 int 子集全部；cc500 保守子集（缺 for，MVP 演示"按能力集裁剪"）。
 */
enum { F_CONST=1<<0, F_VAR=1<<1, F_ARITH=1<<2, F_CMP=1<<3,
       F_LOGIC=1<<4, F_IF=1<<5, F_WHILE=1<<6, F_FOR=1<<7 };
#define CAPS_MINIC (F_CONST|F_VAR|F_ARITH|F_CMP|F_LOGIC|F_IF|F_WHILE|F_FOR)
#define CAPS_CC500 (F_CONST|F_VAR|F_ARITH|F_CMP|F_LOGIC|F_IF|F_WHILE)   /* 保守；待查证 cc500 实子集 */

static int g_caps;                        /* 当前目标能力集：数据非代码 */
static int has(int f) { return g_caps & f; }

#define MAXV 16
static char varnames[MAXV][4];
static int nvars;

#define CLAMP_MAX (1<<28)
static void clamp_iv(long *lo, long *hi) {
    if (*lo < -CLAMP_MAX) *lo = -CLAMP_MAX;
    if (*hi >  CLAMP_MAX) *hi =  CLAMP_MAX;
}

/* 表达式生成：写入 dst（足够大缓冲），返回值域区间。suppress_div：右子树禁用除法以控分母来源。 */
static void expr_gen(char *dst, int depth, long *lo, long *hi, int suppress_div) {
    enum { E_CON, E_VAR, E_ADD, E_SUB, E_MUL, E_DIV, E_CMP, E_LOG };
    enum { MAXDEPTH = 4 };
    if (depth >= MAXDEPTH) {              /* 深度基例：只产叶子 ⇒ 终止递归（防栈溢出） */
        if (has(F_VAR) && nvars > 0 && rbool()) {
            int i = rndi(0, nvars - 1);
            sprintf(dst, "%s", varnames[i]); *lo = 0; *hi = 100;
        } else {
            int c = rndi(1, 100);
            sprintf(dst, "%d", c); *lo = *hi = c;
        }
        return;
    }
    int np = 0, picks[16];
    picks[np++] = E_CON;
    if (has(F_VAR) && nvars > 0) picks[np++] = E_VAR;
    if (has(F_ARITH)) { picks[np++] = E_ADD; picks[np++] = E_SUB; if (nvars > 0) picks[np++] = E_MUL; }
    if (has(F_ARITH) && !suppress_div && nvars > 0) picks[np++] = E_DIV;
    if (has(F_CMP)) picks[np++] = E_CMP;
    if (has(F_LOGIC)) picks[np++] = E_LOG;
    int k = picks[rndi(0, np - 1)];

    switch (k) {
    case E_CON: {
        int c = rndi(1, 100);
        sprintf(dst, "%d", c);
        *lo = *hi = c;
        return;
    }
    case E_VAR: {
        int i = rndi(0, nvars - 1);
        sprintf(dst, "%s", varnames[i]);
        *lo = 0; *hi = 100;                 /* 保守上界；真正值由运行决定 */
        return;
    }
    case E_ADD: case E_SUB: case E_MUL: case E_DIV: {
        const char *op = "+-*/";
        char l[128], r[128]; long ll, lh;
        expr_gen(l, depth + 1, &ll, &lh, 1);
        const char *opc = &op[k - E_ADD];
        if (k == E_DIV || k == E_MUL) {
            sprintf(r, "%d", rndi(2, 9));   /* * 小因子；/ 分母=非零字面量 ⇒ 无除零 */
        } else {
            long rl, rh; expr_gen(r, depth + 1, &rl, &rh, 1);
        }
        sprintf(dst, "(%s%c%s)", l, *opc, r);
        if (k == E_ADD)     { *lo = ll + atol(r); *hi = lh + atol(r); }
        else if (k == E_SUB){ *lo = ll - atol(r); *hi = lh - atol(r); }
        else if (k == E_MUL){ long c = atol(r); *lo = (ll < 0 ? ll : 0) * c; *hi = (lh > 0 ? lh : 0) * c; }
        else                { long c = atol(r); *lo = ll / c; *hi = lh / c; }
        clamp_iv(lo, hi);
        return;
    }
    case E_CMP: {
        static const char *cops[] = { "<", ">", "<=", ">=", "==", "!=" };
        char l[128], r[128]; long ll, lh, rl, rh;
        expr_gen(l, depth + 1, &ll, &lh, 1);
        expr_gen(r, depth + 1, &rl, &rh, 1);
        sprintf(dst, "((%s)%s(%s))", l, cops[rndi(0, 5)], r);
        *lo = 0; *hi = 1;
        return;
    }
    default: {                              /* E_LOG：窄布尔子表达式 ⇒ 结果恒 0/1 */
        char l[128], r[128]; long ll, lh, rl, rh;
        static const char *lops[] = { "&&", "||" };
        expr_gen(l, depth + 1, &ll, &lh, 1);
        if (rbool()) { expr_gen(r, depth + 1, &rl, &rh, 1);
            sprintf(dst, "((%s)%s(%s))", l, lops[rbool()], r); }
        else sprintf(dst, "(!(%s))", l);
        *lo = 0; *hi = 1;
        return;
    }
    }
}

/* 语句生成：赋值 / if-else / while(计数守卫) / for(固定次) —— 全部保证确定性终止、无 UB */
static void stmt_gen(int depth) {
    char b[512]; long lo, hi;
    int kind = rndi(0, 3);
    if (kind == 0) {                        /* 赋值：a[i] = expr */
        int i = rndi(0, nvars - 1);
        expr_gen(b, depth + 1, &lo, &hi, 0);
        printf("  %s=(%s);\n", varnames[i], b);
    } else if (kind == 1) {                 /* if-else：真/假两分支都归一化到 0/1 */
        expr_gen(b, depth + 1, &lo, &hi, 1);
        printf("  if((%s)){ %s=1; } else { %s=0; }\n",
               b, varnames[rndi(0, nvars - 1)], varnames[rndi(0, nvars - 1)]);
    } else if (kind == 2) {                 /* while 计数守卫：_g<20 ⇒ 确定终止 */
        expr_gen(b, depth + 1, &lo, &hi, 1);
        int g = rndi(0, nvars - 1);
        printf("  {int _g; _g=0; while((%s)&&_g<20){ _g=_g+1; %s=%s+1; }}\n",
               b, varnames[g], varnames[g]);
    } else {                                /* for 固定 8 次 */
        int g = rndi(0, nvars - 1);
        printf("  {int _i; for(_i=0;_i<8;_i=_i+1){ %s=%s+1; }}\n", varnames[g], varnames[g]);
    }
}

static void emit_program(int nv, int nstmts) {
    nvars = nv < 1 ? 1 : (nv > MAXV ? MAXV : nv);
    for (int i = 0; i < nvars; i++) sprintf(varnames[i], "v%d", i);
    printf("int main(){\n");
    for (int i = 0; i < nvars; i++) printf("  int %s;\n", varnames[i]);   /* 先声明 */
    for (int i = 0; i < nvars; i++) printf("  %s=%d;\n", varnames[i], rndi(1, 9)); /* 立即初始化 ⇒ 恒已定义 */
    for (int i = 0; i < nstmts; i++) stmt_gen(0);
    char b[512]; long lo, hi;
    expr_gen(b, 0, &lo, &hi, 0);
    printf("  return %s;\n", b);            /* 退码在 [0,255]，gcc 宿主跑 & minicc guest 跑均可比 */
    printf("}\n");
}

int main(int argc, char **argv) {
    unsigned seed = 1; int count = 1; int target = 1; int nv = 3; int nstmts = 6; const char *out = NULL;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--seed")   && i+1<argc) seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count")  && i+1<argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vars")   && i+1<argc) nv = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stmts")  && i+1<argc) nstmts = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--target") && i+1<argc) target = (!strcmp(argv[++i], "cc500")) ? 2 : 1;
        else if (!strcmp(argv[i], "--out")    && i+1<argc) out = argv[++i];
    }
    g_caps = (target == 2) ? CAPS_CC500 : CAPS_MINIC;
    if (count < 1) count = 1;
    for (int n = 1; n <= count; n++) {
        rng_state = seed * 1000003u + (unsigned)n * 2654435761u;   /* 每个样例独立种子流 */
        if (out) {
            char p[512]; snprintf(p, sizeof p, "%s/prog_%03d.c", out, n);
            if (!freopen(p, "w", stdout)) { fprintf(stderr, "[gen] cannot open %s\n", p); return 2; }
        }
        emit_program(nv, nstmts);
        fflush(stdout);
    }
    return 0;
}