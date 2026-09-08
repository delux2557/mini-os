#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_minicc.sh
# V1 minicc 自研编译器（int-only 子集，MIT）回归测试。
#
# 分层（症状对立断言，杜绝假绿）：
#  宿主层 hostminicc：tools/minicc/minicc.c 用宿主 gcc -m32 编成 Linux 程序执行——
#    错误路径与编译成功秒级红绿；另对产物做 objdump 编码断言（除法/取模/入口 stub）。
#   guest 层 QEMU：`micc <src> <out>` 编译并运行，校验运行语义（return code 经由
#    minicc 入口 stub -> sys_exit(ebx) 传回 shell，code==0 为 PASS）。宿主无法运行
#    minicc 产物（int $0x80 为 mini-os 系统调用号，宿主 qemu-i386 无此内核）。
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh

MINICC=tools/minicc/minicc.c
CRT=tools/minicc/host_crt.c
VD="$BUILD/minicc"
mkdir -p "$VD"

echo "== [1/4] 宿主 hostminicc 基座 =="
if ! command -v gcc >/dev/null 2>&1; then echo "[SKIP] 需要宿主 gcc"; exit 2; fi
if ! gcc -m32 -std=gnu99 -O1 -w -fpermissive -o "$VD/hostminicc" "$MINICC" "$CRT" 2>"$VD/hostminicc.log"; then
    echo "[ERR]  hostminicc 编译失败（缺 32 位工具链 gcc-multilib 或 tools/minicc/host_crt.c）；见日志"; tail -8 "$VD/hostminicc.log"; exit 2
fi
RUN=("./$VD/hostminicc")
if ! printf 'int main(){return 0;}' >"$VD/probe.c" \
   || ! "${RUN[@]}" "$VD/probe.c" "$VD/probe.elf" >/dev/null 2>&1; then
    if command -v qemu-i386 >/dev/null 2>&1; then
        RUN=("qemu-i386" "./$VD/hostminicc")
        echo "     宿主无 ia32 exec，改用 qemu-i386（hostminicc 为 32 位二进制）"
    else
        echo "[SKIP] 宿主无 ia32 支持且无 qemu-i386，无法跑 hostminicc"; exit 2
    fi
fi
echo "      hostminicc 就绪"

HOST_FAIL=0; HOST_PASS=0
hrun() { # hrun <name> <src> <expect_rc> <must> <mustn't>
    local name="$1" src="$2" erc="$3" must="$4" mustn="$5" rc out
    printf '%s' "$src" >"$VD/$name.c"
    out=$(timeout 15 "${RUN[@]}" "$VD/$name.c" "$VD/$name.elf" 2>&1); rc=$?
    local ok=1
    [ "$rc" -eq "$erc" ] || ok=0
    { [ -z "$must" ] || echo "$out" | grep -q "$must"; } || ok=0
    if [ -n "$mustn" ] && echo "$out" | grep -q "$mustn"; then ok=0; fi
    if [ "$ok" = 1 ]; then HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 $name (rc=$rc)";
    else echo "[FAIL] 宿主 $name rc=$rc (期望 $erc) must='$must' mustn='$mustn'"; echo "$out"|sed 's/^/        /'; HOST_FAIL=$((HOST_FAIL+1)); fi
}

echo "== [2/4] 宿主错误路径 =="
# 未定义函数 -> 必须 FAIL + undefined function，不得 compiled OK
hrun t_undef 'int main(){return f();}' 1 'undefined function' 'compiled OK'
# 坏数字：0x10 自 V3a 起为合法十六进制（成功路径见下）；123abc -> 必须 FAIL + bad number
hrun t_mixnum 'int main(){int a;a=123abc;return a;}' 1 'bad number' 'compiled OK'
# 未闭合块注释 -> 必须 FAIL + unterminated comment
hrun t_comment 'int main(){/* x' 1 'unterminated comment' 'compiled OK'
# 赋值给非左值 -> 必须 FAIL + assign to non-lvalue
hrun t_nolval 'int main(){int a;a=5;a+1=2;return a;}' 1 'assign to non-lvalue' 'compiled OK'
# 全局初始化非常量 -> 必须 FAIL（V1 仅字面量）
hrun t_ginit 'int g=1+2;int main(){return 0;}' 1 'expected token' 'compiled OK'
echo "== [2a] 宿主指针错误路径（V2b） =="
# 多级指针 -> 必须 FAIL + unsupported multi-level pointer
hrun t_ptrptr 'int main(){int* p;int** q;return 0;}' 1 'multi-level pointer' 'compiled OK'
# 解引用非指针 -> 必须 FAIL + dereference of non-pointer
hrun t_derefni 'int main(){int x;x=5;int y;y=*x;return y;}' 1 'dereference of non-pointer' 'compiled OK'
# 对非左值取地址 -> 必须 FAIL + cannot take address
hrun t_badaddr 'int main(){int* p;p=&(1+2);return 0;}' 1 'cannot take address' 'compiled OK'
# 指针/整型赋值不匹配 -> 必须 FAIL + type mismatch
# （V3a 放宽"右值 int 可赋给指针"以支撑 xmalloc int 地址赋 char*——t_ptrmism 原
#   用例 `p=5` 已合法；改测取地址基类型不匹配：&c 是 char*，赋给 int* 仍必须拒绝）
hrun t_ptrmism 'int main(){char c;c=65;int* p;p=&c;return 0;}' 1 'type mismatch' 'compiled OK'
hrun t_ptrmism2 'int main(){int x;int* p;x=p;return 0;}' 1 'type mismatch' 'compiled OK'
echo "== [2a2] 宿主字符串/char 错误路径（V2c） =="
# 未闭合字符串（EOF/\n 守卫，cc500 F-3 教训）-> 必须 FAIL + unterminated string
hrun t_unstr 'int main(){char* s;s="abc;}' 1 'unterminated string' 'compiled OK'
# 非法转义 -> 必须 FAIL + bad escape
hrun t_badesc 'int main(){char* s;s="\q";return 0;}' 1 'bad escape' 'compiled OK'
# 指针基类型不匹配（int* 收 char*）-> 必须 FAIL + type mismatch
hrun t_badpbase 'int main(){int* p;p="hi";return 0;}' 1 'type mismatch' 'compiled OK'
# 字符串字面量超长（>254 字节）-> 必须 FAIL + string too long
hrun t_strlng "int main(){char* s;s=\"$(printf 'A%.0s' $(seq 1 255))\";return 0;}" 1 'string too long' 'compiled OK'
echo "== [2a3] 宿主数组错误路径（V2d） =="
# 数组名无下标 -> 必须 FAIL + array without subscript
hrun t_anosub 'int main(){int a[2];return a;}' 1 'array without subscript' 'compiled OK'
# 数组初始化 -> 必须 FAIL + array init not supported
hrun t_ainit 'int main(){int a[2]=1;return 0;}' 1 'array init not supported' 'compiled OK'
# 数组大小非正 -> 必须 FAIL + array size must be positive
hrun t_asize0 'int main(){int a[0];return 0;}' 1 'array size must be positive' 'compiled OK'
# 数组参数 -> 必须 FAIL + array parameter
hrun t_aparam 'int f(int a[3]){return 0;}int main(){return 0;}' 1 'array parameter' 'compiled OK'
echo "== [2a4] 宿主循环控制错误路径（do-while/break/continue） =="
# 循环外 break/continue -> 必须 FAIL + break/continue outside loop
hrun t_breakout 'int main(){break;}' 1 'break outside loop' 'compiled OK'
hrun t_continueout 'int main(){continue;}' 1 'continue outside loop' 'compiled OK'
echo "== [2b] 宿主成功路径 =="
# 成功：变量/四则/if/else/while/递归/全局/逻辑，编译层全过
hrun t_arith 'int main(){int a;a=1+2*3-4;return 0;}' 0 'compiled OK' ''
# V3a：十六进制字面量 + 位运算（& | ^ << >> ~）
hrun t_hex 'int main(){int a;a=0x10;if(a==16&&0xff==255&&0x0a==10)return 0;return 1;}' 0 'compiled OK' ''
hrun t_bitops 'int main(){int a;a=6;if((a&3)==2&&(a|1)==7&&(a^2)==4)return 0;return 1;}' 0 'compiled OK' ''
hrun t_shift 'int main(){int a;a=1;if((a<<4)==16&&(48>>4)==3)return 0;return 1;}' 0 'compiled OK' ''
hrun t_bnot 'int main(){int a;a=0;if((~a)==-1)return 0;return 1;}' 0 'compiled OK' ''
hrun t_rec 'int fact(int n){if(n<=1)return 1;return n*fact(n-1);}int main(){return 0;}' 0 'compiled OK' ''
# for（V4）：标准三表达式 + 空条件/空步进 + 嵌套，编译层全过（运行语义见 guest）
hrun t_for 'int main(){int i;int s;s=0;for(i=0;i<10;i=i+1)s=s+i;return 0;}' 0 'compiled OK' ''
hrun t_forempty 'int main(){int i;i=0;for(;;){i=i+1;if(i>3)return 0;}}' 0 'compiled OK' ''
hrun t_fornorestep 'int main(){int i;i=0;for(i=0;i<3;){i=i+1;}return 0;}' 0 'compiled OK' ''
hrun t_fornested 'int main(){int i;int j;int s;s=0;for(i=0;i<3;i=i+1)for(j=0;j<3;j=j+1)s=s+1;return 0;}' 0 'compiled OK' ''
# 循环控制（do-while / break / continue）：宿主编译层全过（运行语义见 guest）
hrun t_do 'int main(){int i;i=0;do{i=i+1;}while(i<5);return 0;}' 0 'compiled OK' ''
hrun t_brk 'int main(){int i;for(i=0;i<10;i=i+1){if(i==3)break;}return 0;}' 0 'compiled OK' ''
hrun t_cont 'int main(){int i;for(i=0;i<10;i=i+1){if(i%2==0)continue;}return 0;}' 0 'compiled OK' ''
hrun t_do_break 'int main(){int i;int s;s=0;i=0;do{s=s+i;i=i+1;if(i==5)break;}while(1);if(s==10)return 0;return 1;}' 0 'compiled OK' ''
hrun t_logic 'int main(){int a;a=1;if(a==1&&!(a==0)||0==1)return 0;return 1;}' 0 'compiled OK' ''
hrun t_global 'int g=7;int main(){int x;x=g;return 0;}' 0 'compiled OK' ''
# 任务1（锁 BUG-039）：GVAR hex 初始化器——0x800a0000 == 2148139008（此前把 x/a 当数字位得垃圾值，
# 自举 CODE_BASE 崩坏源头）。宿主完整版本写后代码；运行语义断言（非仅 compiled OK）。
hrun t_ghex 'int G=0x800a0000;int main(){if(G==2148139008)return 0;return 1;}' 0 'compiled OK' ''
# 0X 大写前缀 + hex 回绕边界（0xFFFFFFFF -> -1）
hrun t_ghexU 'int G=0X1F;int main(){if(G==31)return 0;return 1;}' 0 'compiled OK' ''
hrun t_ghexWrap 'int G=0xFFFFFFFF;int main(){if(G==-1)return 0;return 1;}' 0 'compiled OK' ''
# 指针（V2b）：取地址/解引用/指针参数/指针算术
hrun t_ptr 'int main(){int x;x=5;int* p;p=&x;if(*p==5)return 0;return 1;}' 0 'compiled OK' ''
hrun t_ptrwrite 'int main(){int a;int* p;p=&a;*p=7;if(a==7)return 0;return 1;}' 0 'compiled OK' ''
hrun t_ptrarg 'int f(int* p){return *p;}int main(){int a;a=3;if(f(&a)==3)return 0;return 1;}' 0 'compiled OK' ''
hrun t_ptrarith 'int main(){int a;a=10;int* p;p=&a;if(*(p+0)==10)return 0;return 1;}' 0 'compiled OK' ''
# 字符串/char（V2c）：字面量、转义、char 变量/字面量、char* 指针算术/解引用、syscall3 stub
hrun t_str 'int main(){char* s;s="hello";if(*s==104)return 0;return 1;}' 0 'compiled OK' ''
hrun t_esc 'int main(){char* s;s="a\nb\x41";if(*(s+1)==10&&*(s+3)==65)return 0;return 1;}' 0 'compiled OK' ''
hrun t_char 'int main(){char c;c='\''A'\'';if(c==65)return 0;return 1;}' 0 'compiled OK' ''
hrun t_charptr 'int main(){char c;char* p;p=&c;*p=88;if(c==88)return 0;return 1;}' 0 'compiled OK' ''
hrun t_gchar 'char g='\''B'\'';int main(){if(g==66)return 0;return 1;}' 0 'compiled OK' ''
hrun t_sys3 'int main(){syscall3(1,"hi",0,0);return 0;}' 0 'compiled OK' ''
# 数组（V2d）：局部 int/char、全局、下标表达式、&a[0] 指针
hrun t_arr 'int main(){int a[3];a[0]=1;a[1]=2;a[2]=3;if(a[0]+a[2]==4)return 0;return 1;}' 0 'compiled OK' ''
hrun t_arridx 'int main(){int a[3];a[0]=5;int i;i=1;if(a[i-1]==5)return 0;return 1;}' 0 'compiled OK' ''
hrun t_arrchar 'int main(){char s[3];s[0]=104;s[1]=105;s[2]=0;if(s[0]==104)return 0;return 1;}' 0 'compiled OK' ''
hrun t_arrg 'int a[2];int main(){a[0]=7;a[1]=8;return a[0]+a[1]-15;}' 0 'compiled OK' ''
hrun t_arrptr 'int main(){int a[2];a[0]=9;int* p;p=&a[0];return *p-9;}' 0 'compiled OK' ''
# V3b：复合赋值（+= -= *= /= %=）与前/后缀 ++/--（语法糖，运行语义见 guest/对拍）
hrun t_cpadd 'int main(){int a;a=5;a+=7;return 0;}' 0 'compiled OK' ''
hrun t_cpsub 'int main(){int a;a=5;a-=3;return 0;}' 0 'compiled OK' ''
hrun t_cpmul 'int main(){int a;a=5;a*=4;return 0;}' 0 'compiled OK' ''
hrun t_cpdiv 'int main(){int a;a=20;a/=5;return 0;}' 0 'compiled OK' ''
hrun t_cpmod 'int main(){int a;a=17;a%=5;return 0;}' 0 'compiled OK' ''
hrun t_cpptr 'int main(){int a[2];a[0]=5;int* p;p=&a[0];p+=1;if(*p==5)return 0;return 1;}' 0 'compiled OK' ''
hrun t_incpre 'int main(){int a;int b;a=5;b=++a;return 0;}' 0 'compiled OK' ''
hrun t_decpre 'int main(){int a;int b;a=5;b=--a;return 0;}' 0 'compiled OK' ''
hrun t_incpost 'int main(){int a;int b;a=5;b=a++;return 0;}' 0 'compiled OK' ''
hrun t_decpost 'int main(){int a;int b;a=5;b=a--;return 0;}' 0 'compiled OK' ''
# 非左值复合赋值/自增自减 -> 必须 FAIL（契约：静态报错，绝不出坏码）
hrun t_cpnolval 'int main(){int a;1+=2;return 0;}' 1 'assign to non-lvalue' 'compiled OK'
hrun t_incnolval 'int main(){int a;1++;return 0;}' 1 'increment/decrement of non-lvalue' 'compiled OK'

echo "== [2b2] 外部审计缺陷回归：边界必须受控报错，不得静默坏码 =="
# MC-02：标识符 >31 字符须 FAIL + identifier too long（旧实现写穿 Sym/Node.name[32] → 栈溢出/静默错码）
hrun t_id32 "int main(){int $(printf 'q%.0s' $(seq 1 32));return 0;}" 1 'identifier too long' 'compiled OK'
# 31 字符临界合法，必须不误伤（仍 compiled OK）
hrun t_id31 "int main(){int $(printf 'q%.0s' $(seq 1 31));return 0;}" 0 'compiled OK' ''
# MC-03：局部数组字节数 int 溢出为负 → frame 守卫被绕过；须 FAIL + array too big
hrun t_arrbigL 'int main(){int a[600000000];a[0]=1;return 0;}' 1 'array too big' 'compiled OK'
# MC-03：全局数组溢出为 0 → 相邻全局互相覆盖；须 FAIL + array too big
hrun t_arrbigG 'int g[1073741824];int h=5;int main(){g[0]=1;return 0;}' 1 'array too big' 'compiled OK'
# 合法大全局数组（< 全局上限，未溢出）不得误伤
hrun t_arrg_ok 'int g[100000];int main(){g[0]=7;if(g[99999]==0&&g[0]==7)return 0;return 1;}' 0 'compiled OK' ''
# MC-05：数组长度十六进制解析（旧实现 int g[0x10] 十进制环 → 7210 元素 / 28KB 产物）
hrun t_arrhexL 'int main(){int a[0x4];a[3]=9;if(a[3]==9)return 0;return 1;}' 0 'compiled OK' ''
hrun t_arrhex 'int g[0x10];int main(){g[15]=7;return 0;}' 0 'compiled OK' ''
# MC-05：非数字/空十六进制数组长度仍须拒绝（空 0x 词法层显式拒绝）
hrun t_arrhexbad 'int main(){int a[0xG];return 0;}' 1 'bad number' 'compiled OK'
hrun t_arrhexempty 'int main(){int a[0x];return 0;}' 1 'empty hex literal' 'compiled OK'
# MC-07：八进制字面量（C 语义 010=8，minicc 不实现八进制 → 宁拒不误导）与空 0x 字面量
hrun t_octal 'int main(){int a;a=010;return a;}' 1 'octal literals not supported' 'compiled OK'
hrun t_octal0 'int main(){int a;a=0;return a;}' 0 'compiled OK' ''       # 单个 0 仍合法
hrun t_octal0x 'int main(){int a;a=0x10;return a;}' 0 'compiled OK' ''    # 0x 前置不受八进制拒绝影响
hrun t_emptyhex 'int main(){int a;a=0x;return a;}' 1 'empty hex literal' 'compiled OK'
# MC-06：源码含原始 NUL 字节 -> 必须 FAIL + NUL byte in source（bash 变量不能承载 NUL，直接落盘）
printf 'int main(){return 0;}\x00garbage' >"$VD/t_nul.c"
nout=$(timeout 15 "${RUN[@]}" "$VD/t_nul.c" "$VD/t_nul.elf" 2>&1); nrc=$?
if [ "$nrc" -ne 0 ] && echo "$nout" | grep -q 'NUL byte in source'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 t_nul (rc=$nrc)"
else echo "[FAIL] 宿主 t_nul rc=$nrc 未拒绝 NUL"; echo "$nout"|sed 's/^/        /'; HOST_FAIL=$((HOST_FAIL+1)); fi
# MC-04：函数实参/形参个数不匹配 -> 必须 FAIL + arg count mismatch
hrun t_arity 'int f(int a,int b){return a+b;}int main(){int x;x=f(1);return x;}' 1 'arg count mismatch' 'compiled OK'
hrun t_arity2 'int f(int a,int b){return a+b;}int main(){return f(1);}' 1 'arg count mismatch' 'compiled OK'
hrun t_arity3 'int f(int a){return a;}int main(){return f(1,2);}' 1 'arg count mismatch' 'compiled OK'
hrun t_arityok 'int f(int a,int b){return a+b;}int main(){return f(1,2)-3;}' 0 'compiled OK' ''

echo "== [2b3] 外部审计 MC-08/09 回归：类型收口 + 落尾告警 + 深度守卫 =="
# MC-08#1：char 函数返回类型须保留（旧实现抹平为 TY_INT → 调用点当 int，`int* p=cf()` 被
#   "int→ptr 放宽"错接住）——正常情况下 `char cf()` 调用不得 type mismatch，必须 compiled OK
hrun t_charrc 'char cf(){char c;c=200;return c;}int main(){char v;v=cf();return v;}' 0 'compiled OK' ''
# MC-08#1：int* p = char 函数结果仍须按类型拒绝（返回类型保留后正确报 type mismatch）
hrun t_charrcmis 'char cf(){char c;c=200;return c;}int main(){int* p;p=cf();return 0;}' 1 'type mismatch' 'compiled OK'
# MC-08#2：函数落尾无 return → 告警 control reaches end，且不得误伤（仍 compiled OK）
# （不设 mustn='compiled OK'：落尾告警本就伴随成功编译，二者共存属预期）
hrun t_fallret 'int f(){int x;x=3;}int main(){return 0;}' 0 'control reaches end' ''
# MC-08#2：正常以 return 收尾的函数不得误报
hrun t_okret 'int f(){int x;x=3;return x;}int main(){return 0;}' 0 'compiled OK' ''
# MC-08#2：while(1){...return} 无限循环惯用法不得误报（ends_in_ret 视其为不落尾）
hrun t_while1ret 'int f(){while(1){return 3;}}int main(){return 0;}' 0 'compiled OK' ''
# MC-08#3：main 带形参 → 必须 FAIL + main takes no arguments（入口 stub 恒 0 参 call main）
hrun t_mainarg 'int main(int argc){return 0;}' 1 'main takes no arguments' 'compiled OK'
# MC-08#3：main() 正常零参不误伤
hrun t_main0 'int main(){return 0;}' 0 'compiled OK' ''
# MC-08#1/p+p：双指针相加（C 禁止）须拒绝 pointer + pointer（结果卡/D1 点名的静默接受）
hrun t_ppadd 'int main(){int x;int x2;int* p;p=&x;int* q;q=&x2;p+q;return 0;}' 1 'pointer + pointer' 'compiled OK'
# MC-08#1/p-p：双指针相减（C 属 ptrdiff，本子集不支持）须拒绝 invalid pointer subtraction
hrun t_ppsub 'int main(){int x;int x2;int* p;p=&x;int* q;q=&x2;p-q;return 0;}' 1 'invalid pointer subtraction' 'compiled OK'
# MC-08#1：指针 + 整数 / 整数 + 指针 合法不误伤
hrun t_paddok 'int main(){int x;int* p;p=&x;int* q;q=p+1;return 0;}' 0 'compiled OK' ''
# MC-09：深表达式嵌套须受控报错 expression nesting too deep（旧实现递归打爆 28KB guest 栈 → SIGSEGV）
hrun t_deepnest 'int main(){int a;a=((((((((((((((((((((((((((((((1)))))))))))))))))))))))))));return a-1;}' 1 'expression nesting too deep' 'compiled OK'
# MC-09：深语句块嵌套须受控报错 statement nesting too deep（>STMT_DEPTH_MAX=128 触发）
hrun t_deepblk "int main(){int a;a=0;$(printf '{%.0s' $(seq 1 140))a=a+1;$(printf '}%.0s' $(seq 1 140))return a;}" 1 'statement nesting too deep' 'compiled OK'
# MC-09：普通嵌套（< 上限）不得误伤，仍 compiled OK
hrun t_deepnest_ok 'int main(){int a;a=((((((((1))))))));return a-1;}' 0 'compiled OK' ''

echo "== [2c] 宿主产物编码断言（objdump） =="
# 除法 idiv: pop;xchg;cdq;idiv -> 应含 f7 fb；取模含 89 d0（mov %edx,%eax）
# 注意源码含 % 与 ;，printf 须用 '%s' 格式防格式串解析
printf '%s' 'int main(){int a;a=17/5;int b;b=17%5;return a+b;}' >"$VD/codegen.c"
"${RUN[@]}" "$VD/codegen.c" "$VD/codegen.elf" >/dev/null 2>&1
if objdump -D -b binary -m i386 "$VD/codegen.elf" 2>/dev/null | grep -q 'f7 fb'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 idiv 编码 f7 fb"
else echo "[FAIL] 宿主 idiv 编码未检出 f7 fb"; HOST_FAIL=$((HOST_FAIL+1)); fi
# 入口 stub：call main; mov %eax,%ebx; xor %eax,%eax; int $0x80
# 注意：objdump -b binary 从头线性解码会被 ELF 头数据错位，此处用 od 断言原始字节
if od -A n -t x1 -j 0x54 -N 11 "$VD/codegen.elf" | grep -q '89 c3 31 c0 cd 80'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主入口 stub mov ebx; xor eax; int \$0x80"
else echo "[FAIL] 宿主入口 stub 未检出 89 c3 31 c0 cd 80"; HOST_FAIL=$((HOST_FAIL+1)); fi
# syscall3 stub（V2c）：产物含 int $0x80 包装（入口 stub 之外的第二处 cd 80）
if [ "$(od -A n -t x1 "$VD/t_sys3.elf" | tr -d ' \n' | grep -o 'cd80' | wc -l)" -ge 2 ]; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 syscall3 stub cd 80 ×2"
else echo "[FAIL] 宿主 syscall3 stub 未检出两处 cd 80"; HOST_FAIL=$((HOST_FAIL+1)); fi

echo "== [2c2] 产物体检断言（外部审计 MC-01/05 回归：产物须可被内核 elf_load） =="
# MC-01：空条件 for(;;) 产物 ELF 头必须完好（旧实现把未发射跳转立即数写进 e_ident → 内核拒载）
printf '%s' 'int main(){int i;i=0;for(;;){i=i+1;if(i>3)return 0;}return 1;}' >"$VD/fe.c"
"${RUN[@]}" "$VD/fe.c" "$VD/fe.elf" >/dev/null 2>&1
if [ "$(od -An -tx1 -N 8 "$VD/fe.elf" | tr -d ' \n')" = "7f454c4601010100" ]; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   空条件 for 产物 ELF e_ident 完好 (MC-01)"
else echo "[FAIL] 空条件 for 产物 ELF 头被覆写 (MC-01)"; HOST_FAIL=$((HOST_FAIL+1)); fi
# MC-05：int g[0x10] 应为 16 元素（数据段 64B）；旧实现十进制环 = 7210 元素（~28KB 产物）
printf '%s' 'int g[0x10];int main(){return 0;}' >"$VD/hexarr.c"
"${RUN[@]}" "$VD/hexarr.c" "$VD/hexarr.elf" >/dev/null 2>&1
FSZ=$(od -An -tu4 --endian=little -j 68 -N 4 "$VD/hexarr.elf" | tr -d ' ')
if [ -n "$FSZ" ] && [ "$FSZ" -lt 1000 ]; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   int g[0x10] 产物 p_filesz=$FSZ < 1000 (MC-05)"
else echo "[FAIL] int g[0x10] 产物 p_filesz=${FSZ:-?}（十进制环未修）(MC-05)"; HOST_FAIL=$((HOST_FAIL+1)); fi

echo "== [3/4] guest：micc 编译并运行（return code 语义） =="
if command -v qemu-system-i386 >/dev/null 2>&1; then
    if ! make BUILD="$BUILD" >/dev/null 2>&1; then echo "[FAIL] 内核构建失败"; exit 1; fi
    LOG="$BUILD/minicc_guest.log"; TIN="$BUILD/minicc_in.fifo"; TOUT="$BUILD/minicc_out.fifo"
    QPID=""; CAT_PID=""; GFAIL=0
    cleanup() { exec 9>&- 2>/dev/null || true; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true; rm -f "$TIN" "$TOUT"; }
    trap cleanup EXIT
    rm -f "$LOG" "$TIN" "$TOUT"; mkfifo "$TIN" "$TOUT"
    (cat "$TOUT" > "$LOG") & CAT_PID=$!
    qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none -vga std -no-reboot -no-shutdown \
        -m 64 -nic none -serial stdio -monitor none < "$TIN" > "$TOUT" 2>/dev/null &
    QPID=$!; exec 9>"$TIN"
    gwait() { local desc="$1" re="$2" tmo="${3:-60}" i; for ((i=0;i<tmo*4;i++)); do grep -aq "$re" "$LOG" 2>/dev/null && { echo "[ok]   $desc"; return 0; }; sleep 0.25; done; echo "[FAIL] $desc (缺: $re)"; GFAIL=$((GFAIL+1)); echo "      --- guest 串口尾部（自诊断） ---"; tail -n 8 "$LOG" 2>/dev/null | sed 's/^/      serial| /'; return 1; }
    gsend() { printf '%s\n' "$1" >&9; sleep 0.4; }
    gwait "shell 提示符" "mini-os\$ " 30
    # 运行语义（任一步错 -> 非零退出码 -> micc 判 FAIL；正确 -> code=0 PASS）
    gsend "writefile /mt.c int main(){int a;a=1+2;if(a==3)return 0;return 1;}"
    gsend "micc /mt.c /mt.elf"
    gwait "变量四则编译" "minicc: compiled OK" 40
    gwait "a=1+2==3 return 0" "\[micc\] '/mt.elf' exited code=0 PASS" 40
    # 清理用例文件（FS inode 表 64 项单块：不 rm 会随用例累计耗尽，致后续 create 失败）
    gsend "rm /mt.c"; gsend "rm /mt.elf"
    # 递归 + 参数 + 乘法
    gsend "writefile /mf.c int fact(int n){if(n<=1)return 1;return n*fact(n-1);}int main(){if(fact(5)==120)return 0;return 1;}"
    gsend "micc /mf.c /mf.elf"
    gwait "fact(5)==120 编译" "minicc: compiled OK" 40
    gwait "fact(5)==120 运行" "\[micc\] '/mf.elf' exited code=0 PASS" 40
    gsend "rm /mf.c"; gsend "rm /mf.elf"
    # 全局 + while + 除法/取模 + &&（源码 <128B 避开 writefile 单行截断，同 test_cc500 F-6）
    gsend "writefile /mg.c int g;int main(){int i;i=0;while(i<10){g=g+i;i=i+1;}if(g==45&&17/5==3&&17%5==2)return 0;return 1;}"
    gsend "micc /mg.c /mg.elf"
    gwait "while+div/mod 编译" "minicc: compiled OK" 40
    gwait "g==45 && 17/5==3 && 17%5==2" "\[micc\] '/mg.elf' exited code=0 PASS" 40
    gsend "rm /mg.c"; gsend "rm /mg.elf"
    # for（V4）：三表达式求 0..9 和 ==45，运行语义（<128B 避开 writefile 单行截断）
    gsend "writefile /mfo.c int main(){int i;int s;s=0;for(i=0;i<10;i=i+1)s=s+i;if(s==45)return 0;return 1;}"
    gsend "micc /mfo.c /mfo.elf"
    gwait "for 求和 0..9 编译" "minicc: compiled OK" 40
    gwait "for 求和 0..9==45 运行" "\[micc\] '/mfo.elf' exited code=0 PASS" 40
    gsend "rm /mfo.c"; gsend "rm /mfo.elf"
    # 空条件 for(;;) 真实运行语义（MC-01 回归：旧实现产物头损坏 → 内核拒载，根本跑不起来）
    gsend "writefile /mfe.c int main(){int i;i=0;for(;;){i=i+1;if(i>4)return 0;}return 1;}"
    gsend "micc /mfe.c /mfe.elf"
    gwait "空条件for 编译" "minicc: compiled OK" 40
    gwait "空条件for 运行" "\[micc\] '/mfe.elf' exited code=0 PASS" 40
    gsend "rm /mfe.c"; gsend "rm /mfe.elf"
    # 循环控制（do-while / break / continue）运行语义，均用 heredoc 多行源绕开 128B 单行截断
    # do-while：0..9 求和 ==45（post-test 至少执行一次）
    gsend "writefile <<M /md.c"
    gsend "int main(){int i;int s;s=0;i=0;do{s=s+i;i=i+1;}while(i<10);if(s==45)return 0;return 1;}"
    gsend "M"
    gwait "do-while 源写入" "\[writefile\] '/md.c' wrote" 40
    gsend "micc /md.c /md.elf"
    gwait "do-while 求和编译" "minicc: compiled OK" 40
    gwait "do-while 求和 0..9==45 运行" "\[micc\] '/md.elf' exited code=0 PASS" 40
    gsend "rm /md.c"; gsend "rm /md.elf"
    # break：for 累加 0..4，i==5 早退 -> s==10
    gsend "writefile <<M /mb.c"
    gsend "int main(){int i;int s;s=0;for(i=0;i<10;i=i+1){if(i==5)break;s=s+i;}if(s==10)return 0;return 1;}"
    gsend "M"
    gwait "break 源写入" "\[writefile\] '/mb.c' wrote" 40
    gsend "micc /mb.c /mb.elf"
    gwait "break 早退编译" "minicc: compiled OK" 40
    gwait "break 早退 s==10 运行" "\[micc\] '/mb.elf' exited code=0 PASS" 40
    gsend "rm /mb.c"; gsend "rm /mb.elf"
    # continue：跳偶累奇 1+3+5+7+9 ==25
    gsend "writefile <<M /mc.c"
    gsend "int main(){int i;int s;s=0;for(i=0;i<10;i=i+1){if(i%2==0)continue;s=s+i;}if(s==25)return 0;return 1;}"
    gsend "M"
    gwait "continue 源写入" "\[writefile\] '/mc.c' wrote" 40
    gsend "micc /mc.c /mc.elf"
    gwait "continue 跳偶编译" "minicc: compiled OK" 40
    gwait "continue 累奇 1+3+5+7+9==25 运行" "\[micc\] '/mc.elf' exited code=0 PASS" 40
    gsend "rm /mc.c"; gsend "rm /mc.elf"
    # 嵌套 break：内层 j==1 早退只断内层 -> 每轮外层只加 1，共 3
    gsend "writefile <<M /mn.c"
    gsend "int main(){int i;int j;int s;s=0;for(i=0;i<3;i=i+1){for(j=0;j<3;j=j+1){if(j==1)break;s=s+1;}}if(s==3)return 0;return 1;}"
    gsend "M"
    gwait "嵌套break 源写入" "\[writefile\] '/mn.c' wrote" 40
    gsend "micc /mn.c /mn.elf"
    gwait "嵌套break 编译" "minicc: compiled OK" 40
    gwait "嵌套 break 只断内层 s==3 运行" "\[micc\] '/mn.elf' exited code=0 PASS" 40
    gsend "rm /mn.c"; gsend "rm /mn.elf"
    # 任务9：patch 原语 —— 多行源文件按行替换后 micc 重编
    #   heredoc 先写"错"（return 1）得 FAIL 基线，patch 改第 2 行成 return 0 后重编应 PASS
    #   （源码 <128B 每行，绕开 writefile 单行截断；验证"增行编辑 → 重编"闭环）
    gsend "writefile <<M /mpt.c"
    gsend "int main(){"
    gsend "return 1;}"
    gsend "M"
    gwait "patch 源 heredoc 写入" "\[writefile\] '/mpt.c' wrote 23 bytes" 40
    gsend "micc /mpt.c /mpt.elf"
    gwait "patch 前 FAIL 基线" "\[micc\] '/mpt.elf' exited code=1 FAIL" 40
    gsend "patch /mpt.c 2 return 0;}"
    gwait "patch 改第 2 行" "\[patch\] '/mpt.c' line 2 <- return 0;}" 40
    gsend "micc /mpt.c /mpt.elf"
    gwait "patch 后重编 PASS" "\[micc\] '/mpt.elf' exited code=0 PASS" 40
    gsend "rm /mpt.c"; gsend "rm /mpt.elf"
    # 非零退出码语义：return 1 -> micc 必须报 FAIL（证明退出码真实传回，非恒 0）
    gsend "writefile /m1.c int main(){return 1;}"
    gsend "micc /m1.c /m1.elf"
    gwait "return 1 -> FAIL 语义" "\[micc\] '/m1.elf' exited code=1 FAIL" 40
    gsend "rm /m1.c"; gsend "rm /m1.elf"
    # 编译错误路径：未定义函数 -> compile FAIL（编译器自身 rc=1 传回 shell）
    gsend "writefile /me.c int main(){return g();}"
    gsend "micc /me.c /me.elf"
    gwait "undefined 编译报错" "\[micc\] compile FAIL" 40
    gsend "rm /me.c"; gsend "rm /me.elf"
    # 指针（V2b）：取地址/解引用读写/指针参数/指针算术/多级指针拒绝
    gsend "writefile /pa.c int main(){int x;x=5;int* p;p=&x;if(*p==5)return 0;return 1;}"
    gsend "micc /pa.c /pa.elf"
    gwait "指针取地址解引用" "\[micc\] '/pa.elf' exited code=0 PASS" 40
    gsend "rm /pa.c"; gsend "rm /pa.elf"
    gsend "writefile /pb.c int main(){int a;int* p;p=&a;*p=7;if(a==7)return 0;return 1;}"
    gsend "micc /pb.c /pb.elf"
    gwait "指针解引用写" "\[micc\] '/pb.elf' exited code=0 PASS" 40
    gsend "rm /pb.c"; gsend "rm /pb.elf"
    gsend "writefile /pc.c int f(int* p){return *p;}int main(){int a;a=3;if(f(&a)==3)return 0;return 1;}"
    gsend "micc /pc.c /pc.elf"
    gwait "指针参数 f(&a)" "\[micc\] '/pc.elf' exited code=0 PASS" 40
    gsend "rm /pc.c"; gsend "rm /pc.elf"
    gsend "writefile /pd.c int main(){int a;a=10;int* p;p=&a;if(*(p+0)==10)return 0;return 1;}"
    gsend "micc /pd.c /pd.elf"
    gwait "指针算术 p+0" "\[micc\] '/pd.elf' exited code=0 PASS" 40
    gsend "rm /pd.c"; gsend "rm /pd.elf"
    gsend "writefile /pe.c int main(){int* p;int** q;return 0;}"
    gsend "micc /pe.c /pe.elf"
    gwait "多级指针编译拒绝" "\[micc\] compile FAIL" 40
    gsend "rm /pe.c"; gsend "rm /pe.elf"
    # 字符串/char（V2c）：char 变量、字符串字面量解引用
    gsend "writefile /s1.c int main(){char c;c=65;if(c==65)return 0;return 1;}"
    gsend "micc /s1.c /s1.elf"
    gwait "char 变量" "\[micc\] '/s1.elf' exited code=0 PASS" 40
    gsend "rm /s1.c"; gsend "rm /s1.elf"
    gsend "writefile /s2.c int main(){char* s;s=\"hi\";if(*s==104)return 0;return 1;}"
    gsend "micc /s2.c /s2.elf"
    gwait "字符串解引用" "\[micc\] '/s2.elf' exited code=0 PASS" 40
    gsend "rm /s2.c"; gsend "rm /s2.elf"
    # 可观察 I/O（V2c）：产物经 syscall3 stub 调 SYS_PRINT 输出 "ZY"（源码用 \x 转义，
    # 源码回显/minicc 自身输出均不含 "ZY"，该串只来自产物运行期输出 -> 证明写-编-跑闭环）
    gsend "writefile /s3.c int main(){syscall3(1,\"\\x5a\\x59\",0,0);return 0;}"
    gsend "micc /s3.c /s3.elf"
    gwait "产物输出 ZY（I/O）" "ZY" 40
    gwait "sys_print I/O 运行" "\[micc\] '/s3.elf' exited code=0 PASS" 40
    gsend "rm /s3.c"; gsend "rm /s3.elf"
    # 数组（V2d）：局部读写求和、char 数组、全局数组、&a[0] 指针
    gsend "writefile /a1.c int main(){int a[3];a[0]=1;a[1]=2;a[2]=3;return a[0]+a[1]+a[2]-6;}"
    gsend "micc /a1.c /a1.elf"
    gwait "数组读写求和" "\[micc\] '/a1.elf' exited code=0 PASS" 40
    gsend "rm /a1.c"; gsend "rm /a1.elf"
    gsend "writefile /a2.c int main(){char s[3];s[0]=104;s[1]=105;s[2]=0;if(s[0]==104)return 0;return 1;}"
    gsend "micc /a2.c /a2.elf"
    gwait "char 数组" "\[micc\] '/a2.elf' exited code=0 PASS" 40
    gsend "rm /a2.c"; gsend "rm /a2.elf"
    gsend "writefile /a3.c int a[2];int main(){a[0]=7;a[1]=8;return a[0]+a[1]-15;}"
    gsend "micc /a3.c /a3.elf"
    gwait "全局数组" "\[micc\] '/a3.elf' exited code=0 PASS" 40
    gsend "rm /a3.c"; gsend "rm /a3.elf"
    gsend "writefile /a4.c int main(){int a[2];a[0]=9;int* p;p=&a[0];return *p-9;}"
    gsend "micc /a4.c /a4.elf"
    gwait "数组 &a[0] 指针" "\[micc\] '/a4.elf' exited code=0 PASS" 40
    gsend "rm /a4.c"; gsend "rm /a4.elf"
    # 位运算（V3a）：& | << 混合（含十六进制 0x30）
    gsend "writefile /b1.c int main(){int a;a=6;if((a&3)==2&&(a|1)==7&&(a<<4)==96&&0x30==48)return 0;return 1;}"
    gsend "micc /b1.c /b1.elf"
    gwait "位运算+hex 运行" "\[micc\] '/b1.elf' exited code=0 PASS" 40
    gsend "rm /b1.c"; gsend "rm /b1.elf"
    # V3b：复合赋值 + 前/后缀 ++/-- 运行语义（纯语法糖 → lv=lv op rhs）
    gsend "writefile /ca.c int main(){int a;a=10;a+=5;a-=3;a*=4;a/=2;a%=7;if(a==3)return 0;return 1;}"
    gsend "micc /ca.c /ca.elf"
    gwait "复合赋值 10+=5-=3*=4/=2%%=7==3" "\[micc\] '/ca.elf' exited code=0 PASS" 40
    gsend "writefile /cb.c int main(){int a;int b;a=5;b=++a;if(a==6&&b==6)return 0;return 1;}"
    gsend "micc /cb.c /cb.elf"
    gwait "前缀 ++a 新值" "\[micc\] '/cb.elf' exited code=0 PASS" 40
    gsend "writefile /cc.c int main(){int a;int b;a=5;b=a++;if(a==6&&b==5)return 0;return 1;}"
    gsend "micc /cc.c /cc.elf"
    gwait "后缀 a++ 旧值" "\[micc\] '/cc.elf' exited code=0 PASS" 40
    gsend "writefile /cd.c int main(){int a;int b;a=5;b=--a;if(a==4&&b==4)return 0;return 1;}"
    gsend "micc /cd.c /cd.elf"
    gwait "前缀 --a 新值" "\[micc\] '/cd.elf' exited code=0 PASS" 40
    gsend "writefile /ce.c int main(){int a;int b;a=5;b=a--;if(a==4&&b==5)return 0;return 1;}"
    gsend "micc /ce.c /ce.elf"
    gwait "后缀 a-- 旧值" "\[micc\] '/ce.elf' exited code=0 PASS" 40
    gsend "writefile /cf.c int main(){int a[2];a[0]=5;int* p;p=&a[0];p+=1;a[1]=9;if(*p==9)return 0;return 1;}"
    gsend "micc /cf.c /cf.elf"
    gwait "指针 p+=1 移动" "\[micc\] '/cf.elf' exited code=0 PASS" 40
    if [ "$GFAIL" -gt 0 ]; then echo "[FAIL] guest 层 ${GFAIL} 项未过"; exit 1; fi
    echo "      guest micc 端到端通过"
else
    echo "[warn] 无 qemu-system-i386，跳过 guest 层（宿主层已覆盖错误路径与编码）"
fi

echo
echo "== [3.5/4] Mock 白盒单测（任务4/5） =="
# 白盒：#include minicc.c + -DMINICC_MOCK，setjmp 就地捕获 fail 错误路径。
# 断言宿主层测不到的词法分类 / 算术优先级 AST 形状 / fail 错误消息（共 35 断言）。
# 32 位执行方式与 hostminicc 一致（宿主 ia32 或 qemu-i386 兜底）。
MOCK="$VD/minicc_mock"
if ! gcc -m32 -std=gnu99 -O0 -w -DMINICC_MOCK -o "$MOCK" "tests/test_minicc_mock.c" 2>"$VD/mock_build.log"; then
    echo "[ERR]  Mock 构建失败"; tail -8 "$VD/mock_build.log"; exit 2
fi
MOCK_RUN=("$MOCK")
if ! timeout 5 "$MOCK" >/dev/null 2>&1; then
    if command -v qemu-i386 >/dev/null 2>&1; then MOCK_RUN=("qemu-i386" "$MOCK"); echo "      Mock 经 qemu-i386 运行"; else echo "[SKIP] 无 ia32/qemu 跑 Mock"; exit 0; fi
else
    echo "      Mock 宿主 ia32 直跑"
fi
mout=$(timeout 20 "${MOCK_RUN[@]}" 2>&1); mrc=$?
echo "      ${mout}" | sed 's/^/      mock| /'
if [ "$mrc" -ne 0 ]; then echo "[FAIL] Mock 单测未全绿 (rc=$mrc，输出见上)"; exit 1; fi
echo "      Mock 白盒单测全绿"

echo
echo "== [4/4] 汇总 =="
echo "宿主: PASS=$HOST_PASS FAIL=$HOST_FAIL"
if [ "$HOST_FAIL" -gt 0 ]; then echo "[FAIL] test_minicc 宿主层未全绿"; exit 1; fi
echo "[PASS] test_minicc 全绿"
exit 0
