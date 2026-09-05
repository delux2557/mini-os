/* mini-os/v2-c-kernel/tools/minicc/diffsynth/gen.c
 * 差分对拍生成器核心（mini-Csmith）。
 *
 * 形态：特性表驱动 + 能力集筛选（动态子集）——见 docs/design/minicc-v3-后续任务.md 任务4。
 * 无 UB 三纪律（一票否决）：
 *   1. 声明即初始化：局部 int/数组元素建立即喂已知窄值，指针只 bind 已分配数组基址；
 *   2. 值域限幅 + 分母非零：变量/表达式维护保守区间并夹取到 ±2^28；除法分母来自非零字面量；
 *   3. 循环计数守卫：while 带 _g<20，for 固定 8 次 ⇒ 确定终止。
 *   位运算避开 `<<`（有符号左移溢出是 UB）。
 * 用法：
 *   gen --seed S --count N --target minicc|cc500 [--vars V --stmts S --out DIR]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rng_state;
static unsigned rnd(void) { rng_state ^= rng_state<<13; rng_state ^= rng_state>>17; rng_state ^= rng_state<<5; return rng_state; }
static int rndi(int lo,int hi){ if(hi<=lo)return lo; return lo+(int)(rnd()%(unsigned)(hi-lo+1)); }
static int rbool(void){ return (int)(rnd()&1u); }

enum { F_CONST=1<<0, F_VAR=1<<1, F_ARITH=1<<2, F_CMP=1<<3,
       F_LOGIC=1<<4, F_IF=1<<5, F_WHILE=1<<6, F_FOR=1<<7, F_BIT=1<<8,
       F_GLOBAL=1<<9, F_ARRAY=1<<10, F_PTR=1<<11, F_FUNC=1<<12 };
#define CAPS_MINIC (F_CONST|F_VAR|F_ARITH|F_CMP|F_LOGIC|F_IF|F_WHILE|F_FOR|F_BIT|F_GLOBAL|F_ARRAY|F_PTR|F_FUNC)
/* cc500 保守基座（2026-09-05 实测校准：hostcc 探测 rc——global/array/ptr/for/~/^ 全部拒，
 * 支持 & | << 与 char；故不给 cc500 开 F_GLOBAL/F_ARRAY/F_PTR/F_FOR/F_BIT，与实测一致） */
#define CAPS_CC500 (F_CONST|F_VAR|F_ARITH|F_CMP|F_LOGIC|F_IF|F_WHILE)

static int g_caps;
static int has(int f){ return g_caps & f; }

#define MAXV 16
#define MAXG 2
#define MAXA 2
#define MAXP 2
#define ASZ  5                 /* 数组容量（下标 0..ASZ-1，指针解引用不越界） */
static char varnames[MAXV][4];
static int nvars, ng, na, npa, nf;

#define CLAMP_MAX (1<<28)
static void clamp_iv(long *lo,long *hi){ if(*lo<-CLAMP_MAX)*lo=-CLAMP_MAX; if(*hi>CLAMP_MAX)*hi=CLAMP_MAX; }

/* 可写左值目标池（变量/全局/数组成员/指针解引用）——F_* 控制数量 */
static int nlv; static const char *lvals[64];
static void build_lvals(void){
    int i,j; nlv=0;
    for(i=0;i<nvars && nlv<60;i++) lvals[nlv++]=varnames[i];
    for(i=0;i<ng && nlv<60;i++){ char *s=malloc(24); sprintf(s,"G%d",i); if(nlv<60)lvals[nlv++]=s; }
    for(i=0;i<na && nlv<60;i++){ char *s=malloc(24); sprintf(s,"a%d[%d]",i,rndi(0,ASZ-1)); if(nlv<60)lvals[nlv++]=s; }
    for(i=0;i<npa && nlv<60;i++){ char *s=malloc(24); sprintf(s,"*(p%d+%d)",i,rndi(0,ASZ-1)); if(nlv<60)lvals[nlv++]=s; }
    (void)j;
}
static const char *pick_lval(void){ return lvals[rndi(0,nlv-1)]; }

static void expr_gen(char *dst,int depth,long *lo,long *hi,int suppress_div){
    enum { E_CON,E_VAR,E_GVAR,E_IDX,E_DREF,E_CALL,
           E_ADD,E_SUB,E_MUL,E_DIV,E_CMP,E_LOG,E_AND,E_OR,E_XOR,E_SHR,E_NOT };
    enum { MAXDEPTH=4 };
    if(depth>=MAXDEPTH){
        int c=rndi(1,100); sprintf(dst,"%d",c); *lo=*hi=c; return;
    }
    int np=0,picks[18];
    picks[np++]=E_CON;
    if(has(F_VAR)  && nvars>0) picks[np++]=E_VAR;
    if(has(F_GLOBAL)&&ng>0)    picks[np++]=E_GVAR;
    if(has(F_ARRAY)&&na>0)      picks[np++]=E_IDX;
    if(has(F_PTR)  && npa>0)    picks[np++]=E_DREF;
    if(has(F_FUNC) && nf>0)     picks[np++]=E_CALL;
    if(has(F_ARITH)){ picks[np++]=E_ADD; picks[np++]=E_SUB; if(nvars>0)picks[np++]=E_MUL; }
    if(has(F_ARITH)&&!suppress_div&&nvars>0) picks[np++]=E_DIV;
    if(has(F_CMP))    picks[np++]=E_CMP;
    if(has(F_LOGIC))  picks[np++]=E_LOG;
    if(has(F_BIT)){ picks[np++]=E_AND; picks[np++]=E_OR; picks[np++]=E_XOR; picks[np++]=E_SHR; picks[np++]=E_NOT; }
    int k=picks[rndi(0,np-1)];
    switch(k){
    case E_CON:{ int c=rndi(1,100); sprintf(dst,"%d",c); *lo=*hi=c; return; }
    case E_VAR:{ int i=rndi(0,nvars-1); sprintf(dst,"%s",varnames[i]); *lo=0;*hi=100; return; }
    case E_GVAR:{ int i=rndi(0,ng-1); sprintf(dst,"G%d",i); *lo=0;*hi=100; return; }
    case E_IDX:{ int i=rndi(0,na-1),j=rndi(0,ASZ-1); sprintf(dst,"a%d[%d]",i,j); *lo=0;*hi=100; return; }
    case E_DREF:{ int i=rndi(0,npa-1),k=rndi(0,ASZ-1); sprintf(dst,"*(p%d+%d)",i,k); *lo=0;*hi=100; return; }
    case E_CALL:{ int i=rndi(0,nf-1); char a[128]; long l0,h0v;
        /* 递归参数 = 任意窄值表达式，再按位掩码限幅到非负窄区间 (e & m)：
         * e 由生成器保证无 UB；m ∈ {3,7} ⇒ 结果 ∈ [0,3]/[0,7]，深度/值域有界（sum≤28/fib≤21），
         * 对负 e 仍得非负（& 高位置 0）——深度有界纪律不破。让调用参数多样化以增覆盖。 */
        expr_gen(a,depth+1,&l0,&h0v,0);
        int m=(rbool()?7:3);
        sprintf(dst,"h%d(((%s)&%d))",i,a,m); *lo=0; *hi=100; return; }
    case E_ADD: case E_SUB: case E_MUL: case E_DIV:{
        const char *op="+-*/"; char l[128],r[128]; long ll,lh;
        expr_gen(l,depth+1,&ll,&lh,1); const char *opc=&op[k-E_ADD];
        if(k==E_DIV||k==E_MUL) sprintf(r,"%d",rndi(2,9));
        else { long rl,rh; expr_gen(r,depth+1,&rl,&rh,1); }
        sprintf(dst,"(%s%c%s)",l,*opc,r);
        if(k==E_ADD)*lo=ll+atol(r),*hi=lh+atol(r);
        else if(k==E_SUB)*lo=ll-atol(r),*hi=lh-atol(r);
        else if(k==E_MUL){ long c=atol(r); *lo=(ll<0?ll:0)*c; *hi=(lh>0?lh:0)*c; }
        else { long c=atol(r); *lo=ll/c; *hi=lh/c; }
        clamp_iv(lo,hi); return;
    }
    case E_CMP:{ char l[128],r[128]; long ll,lh,rl,rh;
        static const char *cop[]={"<",">","<=",">=","==","!="};
        expr_gen(l,depth+1,&ll,&lh,1); expr_gen(r,depth+1,&rl,&rh,1);
        sprintf(dst,"((%s)%s(%s))",l,cop[rndi(0,5)],r); *lo=0;*hi=1; return; }
    case E_AND: case E_OR: case E_XOR: case E_SHR: case E_NOT:{
        char l[128],r[128]; long ll,lh;
        expr_gen(l,depth+1,&ll,&lh,0);
        if(k==E_NOT) sprintf(dst,"(~(%s))",l);
        else if(k==E_SHR){ sprintf(r,"%d",rndi(1,3)); sprintf(dst,"((%s)>>%s)",l,r); }
        else { const char *op=(k==E_AND)?"&":(k==E_OR)?"|":"^";
               long rl,rh; expr_gen(r,depth+1,&rl,&rh,0); sprintf(dst,"((%s)%s(%s))",l,op,r); }
        *lo=-CLAMP_MAX; *hi=CLAMP_MAX; clamp_iv(lo,hi); return; }
    default:{ char l[128],r[128]; long ll,lh,rl,rh;
        static const char *lop[]={"&&","||"};
        expr_gen(l,depth+1,&ll,&lh,1);
        if(rbool()){ expr_gen(r,depth+1,&rl,&rh,1); sprintf(dst,"((%s)%s(%s))",l,lop[rbool()],r); }
        else { sprintf(dst,"(!(%s))",l); }
        *lo=0;*hi=1; return; }
    }
}

static void stmt_gen(int depth){
    char b[512]; long lo,hi;
    int kind=rndi(0,3);
    const char *lv=pick_lval();
    if(kind==0){ expr_gen(b,depth+1,&lo,&hi,0); printf("  %s=(%s);\n",lv,b); }
    else if(kind==1){ expr_gen(b,depth+1,&lo,&hi,1);
        printf("  if((%s)){ %s=1; } else { %s=0; }\n",b,lv,pick_lval()); }
    else if(kind==2){ expr_gen(b,depth+1,&lo,&hi,1);
        printf("  {int _g; _g=0; while((%s)&&_g<20){ _g=_g+1; %s=%s+1; }}\n",b,lv,lv); }
    else{ printf("  {int _i; for(_i=0;_i<8;_i=_i+1){ %s=%s+1; }}\n",lv,lv); }
}

/* 递归辅助函数：固定安全模板，纯函数、参数作深度界限。
 * h0 = sum(n)=n<=0?0:n+h0(n-1)（线性递归，深压栈帧）；h1 = fib(n)=n<=1?1:h1(n-1)+h1(n-2)。
 * 只用 int/if-else/return/本地声明/算术/递归——minicc 子集可编，且无副作用、确定终止。 */
static void emit_funcs(void){
    nf = has(F_FUNC)?2:0;
    if(nf>=1) printf("int h0(int n){ int r; if(n<=0){ r=0; } else { r=n+h0(n-1); } return r; }\n");
    if(nf>=2) printf("int h1(int n){ int r; if(n<=1){ r=1; } else { r=h1(n-1)+h1(n-2); } return r; }\n");
}

static void emit_program(int nv,int nstmts){
    nvars = nv<1?1:(nv>MAXV?MAXV:nv);
    ng  = has(F_GLOBAL)?MAXG:0;
    na  = has(F_ARRAY)?MAXA:0;
    npa = has(F_PTR)&&na>0?MAXP:0;
    for(int i=0;i<nvars;i++) sprintf(varnames[i],"v%d",i);
    emit_funcs();
    if(ng>0){ printf("int G0;\n"); if(ng>1) printf("int G1;\n"); }
    printf("int main(){\n");
    for(int i=0;i<nvars;i++) printf("  int %s;\n",varnames[i]);
    for(int i=0;i<nvars;i++) printf("  %s=%d;\n",varnames[i],rndi(1,9)); /* 声明即初始化 */
    for(int i=0;i<na;i++){ printf("  int a%d[%d];\n",i,ASZ);
        for(int j=0;j<ASZ;j++) printf("  a%d[%d]=%d;\n",i,j,rndi(1,9)); } /* 全元素初始化 → 防读未初始化 */
    for(int i=0;i<npa;i++){ int bm=i%na; printf("  int* p%d; p%d=&a%d[0];\n",i,i,bm); } /* 指针 bind 数组基址 */
    build_lvals();
    for(int i=0;i<nstmts;i++) stmt_gen(0);
    char b[512]; long lo,hi;
    expr_gen(b,0,&lo,&hi,0);
    printf("  return %s;\n",b);
    printf("}\n");
}

int main(int argc,char **argv){
    unsigned seed=1; int count=1,target=1,nv=4,nstmts=6; const char *out=NULL;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--seed")&&i+1<argc) seed=(unsigned)atoi(argv[++i]);
        else if(!strcmp(argv[i],"--count")&&i+1<argc) count=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--vars")&&i+1<argc) nv=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--stmts")&&i+1<argc) nstmts=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--target")&&i+1<argc) target=(!strcmp(argv[++i],"cc500"))?2:1;
        else if(!strcmp(argv[i],"--out")&&i+1<argc) out=argv[++i];
    }
    g_caps=(target==2)?CAPS_CC500:CAPS_MINIC;
    if(count<1) count=1;
    for(int n=1;n<=count;n++){
        rng_state=seed*1000003u+(unsigned)n*2654435761u;
        if(out){ char p[512]; snprintf(p,sizeof p,"%s/prog_%03d.c",out,n);
            if(!freopen(p,"w",stdout)){ fprintf(stderr,"[gen] %s\n",p); return 2; } }
        emit_program(nv,nstmts); fflush(stdout);
    }
    return 0;
}