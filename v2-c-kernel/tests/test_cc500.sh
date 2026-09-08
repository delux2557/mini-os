#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/test_cc500.sh
# v0.32 cc500 自举编译器缺陷回归（F-1 关系运算残缺/F-2 未定义符号静默/F-3 未闭合字符串自噬）
#
# 设计（症状对立断言："新症状必须出现 + 旧症状必须缺席"，杜绝假绿）：
#  宿主层 hostcc：把 tools/cc500/cc500.c 直接用宿主 gcc -m32 编成 Linux 程序执行——
#    缺陷与内核无关，秒级红绿；每项同时断言 "must 出现" 与 "mustn't 缺席"。
#   guest 层 QEMU：ccboot 自举不动点（P1==P2）+ 关系运算 < 运行语义（需内核把新 cc500.c 嵌入）。
set -u
cd "$(dirname "$0")/.." || exit 1
source tests/_build_env.sh

CC500=tools/cc500/cc500.c
CRT=tools/cc500/host_crt.c
VD="$BUILD/cc500"
mkdir -p "$VD"

echo "== [1/4] 宿主 hostcc 基座 =="
if ! command -v gcc >/dev/null 2>&1; then echo "[SKIP] 需要宿主 gcc"; exit 2; fi
if ! gcc -m32 -std=gnu99 -O1 -w -fpermissive -o "$VD/hostcc" "$CC500" "$CRT" 2>"$VD/hostcc.log"; then
    echo "[ERR]  hostcc 编译失败（环境/仓库问题：缺 32 位工具链 gcc-multilib 或 tools/cc500/host_crt.c）；见日志"; tail -8 "$VD/hostcc.log"; exit 2
fi
RUN=("./$VD/hostcc")
if ! printf 'int main(){return 0;}' >"$VD/probe.c" \
   || ! "${RUN[@]}" "$VD/probe.c" "$VD/probe.elf" >/dev/null 2>&1; then
    if command -v qemu-i386 >/dev/null 2>&1; then
        RUN=("qemu-i386" "./$VD/hostcc")
        echo "     宿主无 ia32 exec，改用 qemu-i386（hostcc 为 32 位二进制）"
    else
        echo "[SKIP] 宿主无 ia32 支持且无 qemu-i386，无法跑 hostcc"; exit 2
    fi
fi
echo "      hostcc 就绪"

HOST_FAIL=0; HOST_PASS=0
hrun() { # hrun <name> <src> <expect_rc> <must> <mustn't>
    local name="$1" src="$2" erc="$3" must="$4" mustn="$5" rc out
    printf '%s' "$src" >"$VD/$name.c"
    # timeout 兜底：缺陷若退化为死循环（旧 BUG-048 块注释 EOF 死循环）不能挂死 CI
    out=$(timeout 15 "${RUN[@]}" "$VD/$name.c" "$VD/$name.elf" 2>&1); rc=$?
    local ok=1
    [ "$rc" -eq "$erc" ] || ok=0
    { [ -z "$must" ] || echo "$out" | grep -q "$must"; } || ok=0
    if [ -n "$mustn" ] && echo "$out" | grep -q "$mustn"; then ok=0; fi
    if [ "$ok" = 1 ]; then HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 $name (rc=$rc)";
    else echo "[FAIL] 宿主 $name rc=$rc (期望 $erc) must='$must' mustn='$mustn'"; echo "$out"|sed 's/^/        /'; HOST_FAIL=$((HOST_FAIL+1)); fi
}

echo "== [2/4] 宿主 T 系列（F-1/F-2/F-3 症状对立）=="
# F-2：只声明未定义函数 -> 必须 FAIL + undefined symbol，且不得再 "compiled OK"
hrun t4_undef 'int sys_print(char*s);int main(){sys_print("hi");return 0;}' \
     1 'undefined symbol' 'compiled OK'
# F-2 负对照：声明+同文件定义 -> 必须 OK（防误报）
hrun t4_ok 'int sys_print(char*s);int main(){sys_print("hi");return 0;}int sys_print(char*s){return 0;}' \
     0 'compiled OK' ''
# F-3：未闭合字符串 -> 必须 FAIL + bad string，且不是 SIGSEGV(139)
hrun t6_bad 'int sys_print(char*s);int main(){sys_print("unterminated' \
     1 'bad string' ''
# BUG-048：未闭合块注释 -> 必须 FAIL（干净报错 rc=1），不得死循环（timeout 兜底）
hrun t_bcomm 'int main(){/* unterminated comment' \
     1 'cc500: error' 'compiled OK'
# F-4：空/仅注释源（无任何函数=无入口）——必须 FAIL 干净报错（undefined symbol），
# 不得 SIGSEGV(139)（旧缺陷：token 惰性分配，空源永不 takechar->写 NULL）也不得骗 compiled OK
hrun t_empty '' 1 'undefined symbol' 'compiled OK'
hrun t_comment_nofn '/* only a comment, no function */' 1 'undefined symbol' 'compiled OK'
# M9：十六进制字面量 0x…（2026-09-07）——0x10 由 BUG-049 的拒绝态转正为 0x10==16
# （编译路径；数值/大值语义在 [3/4] guest 段断言；编码锁定见 HEX_PAT）。
hrun t_hex_ok  'int main(){return 0x10;}' 0 'compiled OK' ''
hrun t_hex_a2f 'int main(){int x;x=0xa;x=0xff;return x;}' 0 'compiled OK' ''
hrun t_hex_mix 'int main(){int x;x=0x10+0x20;return x;}' 0 'compiled OK' ''
hrun t_hex_zero 'int main(){int x;x=0x0;return x;}' 0 'compiled OK' ''
# 供上方 HEX_PAT 编码断言取材：唯一 hex 大值 → 产物立即数即 0xdeadbeef
hrun t_heximm 'int main(){return 0xdeadbeef;}' 0 'compiled OK' ''
# BUG-049 遗留负对照：非 0x 前缀的十进制混入字母仍须 FAIL 且报出错 token
# 纯非法 hex 字符（0x1z）也 clean error，不静默算错、不骗 compiled OK
hrun t_hex_bad  'int main(){return 0x1z;}' 1 '0x1z' 'compiled OK'
# 0x-1 实为 0x-1 即 0-1=-1（'-' 后总是新子表达式，无数字缺口路径）——正例而非负例
hrun t_hex_sign 'int main(){int x;x=0x-1;return x;}' 0 'compiled OK' ''
hrun t_mixalpha 'int main(){return 123abc;}' \
     1 '123abc' 'compiled OK'
# F-1：关系 < / > / >= / <= 均须能编译通过（编码与语义下方另行实证）
hrun t_lt 'int main(){int i;i=0;while(i<3){i=i+1;}return 0;}' 0 'compiled OK' ''
hrun t_gt 'int main(){int i;i=9;while(i>3){i=i-1;}return 0;}' 0 'compiled OK' ''
hrun t_ge 'int main(){int i;i=3;while(i>=3){i=i-1;}return 0;}' 0 'compiled OK' ''
hrun t_le 'int main(){int i;i=0;while(i<=3){i=i+1;}return 0;}' 0 'compiled OK' ''
# M1：for / do-while 语法与 codegen（语义在 [3/4] guest 段运行断言；此处先证编译路径可用）
hrun t_for_ok 'int main(){int i;int s;s=0;for(i=0;i<3;i=i+1){s=s+2;}return s;}' 0 'compiled OK' ''
hrun t_do_ok 'int main(){int i;i=0;do{i=i+1;}while(i<3);return i;}' 0 'compiled OK' ''
# M2：一元 - ! ~ 与位异或 ^（语义在 [3/4] guest 段运行断言；此处先证编译路径可用）
hrun t_neg_ok 'int main(){int x;x=5;x=-x;return x;}' 0 'compiled OK' ''
hrun t_not_ok 'int main(){int x;x=0;x=!x;return x;}' 0 'compiled OK' ''
hrun t_bnot_ok 'int main(){int x;x=5;x=~x;return x;}' 0 'compiled OK' ''
hrun t_xor_ok 'int main(){int x;x=5;x=x^3;return x;}' 0 'compiled OK' ''
# M3：乘/除/模与优先级（语义在 [3/4] guest 段运行断言；此处先证编译路径与优先级解析可用）
hrun t_mul_ok 'int main(){int x;x=7*6;return x;}' 0 'compiled OK' ''
hrun t_divmod_ok 'int main(){int x;x=17/5;x=17%5;return x;}' 0 'compiled OK' ''
hrun t_prec_ok 'int main(){int x;x=2+3*4;x=(2+3)*4;return x;}' 0 'compiled OK' ''
# M4：复合赋值 += -= *= %= 编译路径（语义在 [3/4] guest 段运行断言；链式 5+3-1,*2,%5==4）
hrun t_ca_ok 'int main(){int x;x=5;x+=3;x-=1;x*=2;x%=5;return x;}' 0 'compiled OK' ''
hrun t_ca_rhs 'int main(){int x;x=5;x+=2*3;return x;}' 0 'compiled OK' ''
# M4：下标 lvalue 复合赋值编译路径（lvalue 单次求值的运行断言在 guest 段 t_calf）
hrun t_ca_lval 'int g;int f(){g=g+1;return 0;}int main(){char *s;s="A";g=0;s[f()]+=1;return g;}' 0 'compiled OK' ''
# M4 症状对立素材：x+=3 若被吞成 x=3（旧缺陷：+= 不在 token 合成表，被拆成 = +），
# 产物必无 load 旧值指令——下方 CA_PAT 断言即红
hrun t_ca_load 'int main(){int x;x=5;x+=3;return x;}' 0 'compiled OK' ''
# M4 纪律#2：非 lvalue 目标（3 += 1）必须 error 不静默（不得产出坏产物）
hrun t_ca_nolv 'int main(){int x;x=3+=1;return x;}' 1 'cc500: error' 'compiled OK'
# M9b：/= <<= >>= &= |= ^= 复合赋值补齐（2026-09-07）
# 编译路径（语义在 [3/4] guest 段断言）；'/' 后随 '=' 现成 '/=' 独立 operator，不再按 '/' '='
# 拆开报错。负对照：非 lvalue 目标（3/=2、3<<=1）仍须 error 不静默。
hrun t_ca_div  'int main(){int x;x=17;x/=5;return x;}' 0 'compiled OK' ''
hrun t_ca_x2   'int main(){int x;x=5;x*=4;x/=2;return x;}' 0 'compiled OK' ''
hrun t_ca_shl  'int main(){int x;x=1;x<<=4;x>>=2;return x;}' 0 'compiled OK' ''
hrun t_ca_bit  'int main(){int x;x=0;x|=3;x&=5;x^=6;return x;}' 0 'compiled OK' ''
hrun t_ca_rval 'int main(){int x;x=5;x/=2*3;x<<=1+1;return x;}' 0 'compiled OK' ''
hrun t_ca_nodiv 'int main(){int x;x=5;3/=2;return x;}' 1 'cc500: error' 'compiled OK'
hrun t_ca_noshift 'int main(){int x;x=5;3<<=1;return x;}' 1 'cc500: error' 'compiled OK'
# ---- M5：break / continue 循环控制（2026-09-06）----
# 编译路径：for+break（0..9 遇 5 断，求和 10）与 for+continue（跳过奇数，偶数求和 20）——
# 语义断言在 [3/4] guest 段；此处仅验编译路径（rc=0=compiled OK）。
hrun t_brk 'int main(){int i;int s;s=0;for(i=0;i<10;i=i+1){if(i==5)break;s=s+i;}if(s==10)return 0;return 1;}' 0 'compiled OK' ''
hrun t_cnt 'int main(){int i;int s;s=0;for(i=0;i<10;i=i+1){if(i%2)continue;s=s+i;}if(s==20)return 0;return 1;}' 0 'compiled OK' ''
hrun t_dobc 'int main(){int i;int s;s=0;i=0;do{i=i+1;if(i==3)continue;if(i==5)break;s=s+i;}while(1);if(s==7)return 0;return 1;}' 0 'compiled OK' ''
# M5 纪律#1：循环外 break / continue 必须 error 不静默（不得产出坏码）
hrun t_brk_oob 'int main(){int i;i=0;break;i=i+1;return 0;}' 1 'cc500: error' 'compiled OK'
hrun t_cnt_oob 'int main(){int i;i=0;continue;i=i+1;return 0;}' 1 'cc500: error' 'compiled OK'
# ---- M6：短路逻辑 && / ||（2026-09-06）----
# 编译路径四例（短路语义 / 右操作数跳过在 [3/4] guest 段运行断言；症状对立编码见 AND_PAT/OR_PAT）
hrun t_and_ok 'int main(){int a;a=3;if((a>0)&&(a<5))return 0;return 1;}' 0 'compiled OK' ''
hrun t_or_ok  'int main(){int a;a=0;if((a>0)||(a==0))return 0;return 1;}' 0 'compiled OK' ''
hrun t_andor  'int main(){int a;a=3;if(a>0&&a<5||a==9)return 0;return 1;}' 0 'compiled OK' ''
hrun t_andll  'int main(){int a;int b;a=1;b=1;if(a&&b&&a)return 0;return 1;}' 0 'compiled OK' ''
# ---- M7：?: 三目（2026-09-06）----
# 编译路径（运行时语义/分支互斥在 [3/4] guest 段）；t_q_ok 无 if/while/&&/||/for/do，
# 故其产物唯一条件跳 je(0f 84) 必来自三目（见下方 Q_PAT 症状对立断言）。
hrun t_q_ok    'int main(){int a;int b;a=3;b=(a>1?5:6);return b;}' 0 'compiled OK' ''
hrun t_q_nested 'int main(){int a;a=(1?(2?3:4):5);return a;}' 0 'compiled OK' ''
hrun t_q_rhs   'int main(){int a;int b;a=2;b=(a?1:2);b=(b>1?a:0);return b;}' 0 'compiled OK' ''
# ---- M8：++/-- 自增自减（2026-09-06）----
hrun t_inc_ok  'int main(){int i;i=5;i++;i++;return i;}' 0 'compiled OK' ''
hrun t_dec_ok  'int main(){int i;i=5;i--;i--;return i;}' 0 'compiled OK' ''
hrun t_incpre  'int main(){int i;int j;i=5;j=++i;return j;}' 0 'compiled OK' ''
hrun t_incpost 'int main(){int i;int r;i=5;r=i++;return r;}' 0 'compiled OK' ''
# M8 症状对立：非 lvalue 自增（3++）必须 error 不静默（不得骗 compiled OK）
hrun t_inc_nolv 'int main(){int i;i=3++;return i;}' 1 'cc500: error' 'compiled OK'
# OBS-CC-1（护栏）：递归下降深度上限——>512 层嵌套必须被 error() 拒绝（rc=1、
# 出现 cc500: error 且不得 compiled OK），不得耗尽栈/死循环/击穿。护栏靠 cc_depth
# 编译期计数判定、与栈大小无关，hostcc 秒级可复现，落在宿主层。
python3 - "$VD/deep.c" <<'PY'
import sys
N = 640                              # > CC_DEPTH_MAX(512)，保证触发护栏
with open(sys.argv[1], 'w') as f:
    f.write('void f(){')
    f.write('{' * N)
    f.write('0;')
    f.write('}' * N)
    f.write('}\n')
PY
dout=$(timeout 15 "${RUN[@]}" "$VD/deep.c" "$VD/deep.elf" 2>&1); drc=$?
if [ "$drc" = 1 ] && echo "$dout" | grep -q 'cc500: error' && ! echo "$dout" | grep -q 'compiled OK'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 OBS-CC-1 护栏拒绝深嵌套 (rc=$drc)"
else
    echo "[FAIL] 宿主 OBS-CC-1 rc=$drc"; echo "$dout" | sed 's/^/        /'; HOST_FAIL=$((HOST_FAIL+1))
fi
# F-1：新增关系运算机器码编码锁定（setl=0f 9c，确认操作数序 != 照抄）—— 若符号缺失/错编码则该断言红
LT_PAT='0f 9c'
if objdump -D -b binary -m i386 "$VD/t_lt.elf" 2>/dev/null | grep -q "$LT_PAT"; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 < 编码确认 $LT_PAT (setl)"
else
    echo "[FAIL] 宿主 < 编码未检出 $LT_PAT"; HOST_FAIL=$((HOST_FAIL+1))
fi
# M4：+= 必须先 load 旧值再运算（mov (%ebx),%eax = 8b 03）——错编成 x=3（旧缺陷
# 静默吞并）的产物无此 load，症状对立断言锁定 M4 修复
CA_PAT='8b 03'
if objdump -D -b binary -m i386 "$VD/t_ca_load.elf" 2>/dev/null | grep -q "$CA_PAT"; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 += load 旧值确认 ($CA_PAT)"
else
    echo "[FAIL] 宿主 += 未检出 load ($CA_PAT)——疑似被吞成 ="; HOST_FAIL=$((HOST_FAIL+1))
fi
# M9b：/= 复合赋值编码锁定——x/=2 须含 idiv(eax/=ebx, f7 fb) 除法路径；若 '/' 后 '=' 仍被
# 拆分（/ 后按注释失败、'=' 游离）则无 f7 fb 且语法 err（症状对立）。shift 补 <<= 的 shl d3 e0。
if objdump -D -b binary -m i386 "$VD/t_ca_div.elf" 2>/dev/null | grep -q 'f7 fb'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 /= idiv(eax/=ebx, f7 fb) 编码确认"
else
    echo "[FAIL] 宿主 /= 未检出 idiv f7 fb（疑似 '/' '=' 被拆开）"; HOST_FAIL=$((HOST_FAIL+1))
fi
if objdump -D -b binary -m i386 "$VD/t_ca_shl.elf" 2>/dev/null | grep -q 'd3 e0'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 <<= shl %cl d3 e0 编码确认"
else
    echo "[FAIL] 宿主 <<= 未检出 shl d3 e0（疑似移位量/操作数据序错）"; HOST_FAIL=$((HOST_FAIL+1))
fi
# M6：短路跳转编码锁定——&& 产物须含 test;je(0f 84)、|| 须含 test;jne(0f 85)。
# 若实现退化成按位 &(0f 21) / |(0f 09) 贪心求值或退化成无跳转，则断言红；症状对立锁定短路语义。
if objdump -D -b binary -m i386 "$VD/t_and_ok.elf" 2>/dev/null | grep -q '0f 84'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 && 短路 je(0f 84) 编码确认"
else
    echo "[FAIL] 宿主 && 未检出 0f 84（疑似退化为按位&/非短路）"; HOST_FAIL=$((HOST_FAIL+1))
fi
if objdump -D -b binary -m i386 "$VD/t_or_ok.elf" 2>/dev/null | grep -q '0f 85'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 || 短路 jne(0f 85) 编码确认"
else
    echo "[FAIL] 宿主 || 未检出 0f 85（疑似退化为按位|/非短路）"; HOST_FAIL=$((HOST_FAIL+1))
fi
# M7：三目条件跳编码锁定——t_q_ok 无 if/while/&&/||/for/do，其 je(0f 84) 必来自三目的
# "条件假跳假分支"。若实现退化成贪心/无跳转则断言红（症状对立：缺 M7 则无此条件跳）。
if objdump -D -b binary -m i386 "$VD/t_q_ok.elf" 2>/dev/null | grep -q '0f 84'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 ?: 条件跳 je(0f 84) 编码确认"
else
    echo "[FAIL] 宿主 ?: 未检出 0f 84（疑似未发条件跳）"; HOST_FAIL=$((HOST_FAIL+1))
fi
# M8：后缀值=旧值编码锁定——r=i++ 产物须含 mov %ecx,%eax(89 c8) 恢复旧值；若后缀被当成
# 折扣(返回新值)则无此恢复，断言红（症状对立）。前缀版本 j=++i 无 89 c8。
if objdump -D -b binary -m i386 "$VD/t_incpost.elf" 2>/dev/null | grep -q '89 c8'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 ++ 后缀旧值 编码确认 (89 c8 恢复旧值)"
else
    echo "[FAIL] 宿主 ++ 未检出 89 c8（后缀疑似被当折扣/返新值）"; HOST_FAIL=$((HOST_FAIL+1))
fi
# M9：hex 立即数编码锁定——return 0xdeadbeef 的产物须含 mov $0xdeadbeef,%eax
#（b8 ef be ad de）。若 hex 解析错位（如按 0xde+adbe 拆开、或把 0xdeadbeef 当十进制）
# 则立即数不同，objdump 无 'de ad be ef' 字节序 → 断言红（症状对立）。
if objdump -D -b binary -m i386 "$VD/t_heximm.elf" 2>/dev/null | grep -q 'ef be ad de'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 hex 立即数 0xdeadbeef 编码确认 (b8 ef be ad de)"
else
    echo "[FAIL] 宿主 hex 未检出立即数 0xdeadbeef（疑似解析错位）"; HOST_FAIL=$((HOST_FAIL+1))
fi
# OBS-CC-5（输入上限抬升）：构造 >64KB 源（巨型注释 + main），必须编译成功 rc=0 + compiled OK，
# 且不得再出现 "input too big" 截断报错（旧缺陷：open_input 定长 malloc(65536)，读满探 1 字节
# 判截断后显式拒绝）。若实现退回定长/仍未扩容，>64KB 源即被拒 rc=1（症状对立）。
python3 - "$VD/big.c" <<'PY'
import sys
with open(sys.argv[1], 'w') as f:
    f.write('/* OBS-CC-5 扩容回归：')
    f.write('A' * 70000)          # > 65536，必触发旧定长拒绝，撞新扩容路径
    f.write('*/\n')
    f.write('int main(){return 0;}\n')
PY
bout=$(timeout 20 "${RUN[@]}" "$VD/big.c" "$VD/big.elf" 2>&1); brc=$?
if [ "$brc" = 0 ] && echo "$bout" | grep -q 'compiled OK' && ! echo "$bout" | grep -q 'input too big'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 OBS-CC-5 输入 >64KB 编译通过 (rc=$brc)"
else
    echo "[FAIL] 宿主 OBS-CC-5 rc=$brc（>64KB 源未扩容编译）"; echo "$bout" | sed 's/^/        /'; HOST_FAIL=$((HOST_FAIL+1))
fi
# ---- M11：goto / labels（2026-09-07）----
# 编译路径：前向 goto 跳过代码、后向 goto 构成循环、链式多标签依次跳过——均须 rc=0 compiled OK。
# 负对照（症状对立）：goto 指向从未定义的标签 / 同标签重复定义，均须 rc=1 + 干净 cc500: error，
# 且不得骗 compiled OK（若前向回填基建坏，未定义标签会静默落下或错跳）。
hrun t_gfwd  'int main(){int a;a=7;goto skip;a=99;skip:if(a==7)return 0;return 1;}' 0 'compiled OK' ''
hrun t_gbwd  'int main(){int i;i=0;top:i=i+1;if(i<5)goto top;if(i==5)return 0;return 1;}' 0 'compiled OK' ''
hrun t_gmulti 'int main(){int a;int s;s=0;goto m0;s=1;m0:goto m1;s=2;m1:goto m2;s=3;m2:if(s==0)return 0;return 1;}' 0 'compiled OK' ''
hrun t_gundef 'int main(){goto nod;return 0;done:;}' 1 'cc500: error' 'compiled OK'
hrun t_gdup   'int main(){int a;a=1;dup:a=2;dup:return a;}' 1 'cc500: error' 'compiled OK'
# M11 编码锁定：前向 goto-only 程序（无 if/while/for/?:）的唯一无条件 jmp 即 goto。
# 若前向回填坏（挂起未解/落错位）→ rel32=0 退化成顺落（e9 00 00 00 00）；正确则跳过 a=9
# 得正位移 e9 15 00 00 00（症状对立）。
hrun t_gw 'int main(){int a;a=0;goto ld;a=9;ld:return a;}' 0 'compiled OK' ''
if objdump -D -b binary -m i386 "$VD/t_gw.elf" 2>/dev/null | grep -q 'e9 15 00 00 00'; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 goto 前向回填 jmp e9 15 编码确认（跳过 a=9）"
else
    echo "[FAIL] 宿主 goto 前向未检出正位移 jmp（疑似回填未解/退化顺落）"; HOST_FAIL=$((HOST_FAIL+1))
fi
# ---- M12：switch/case/default（2026-09-07）----
# 编译路径：命中/默认/贯通(nested fall-through)/负常量/嵌套 switch/switch 内循环 break 均须 OK。
# 负对照（症状对立）：switch 外 case 必须 error rc=1 不得骗 compiled OK。
hrun t_sw1   'int main(){int x;int r;x=2;r=0;switch(x){case 1:r=10;break;case 2:r=20;break;default:r=99;}if(r==20)return 0;return 1;}' 0 'compiled OK' ''
hrun t_sw2   'int main(){int x;int r;x=7;r=0;switch(x){case 1:r=10;break;case 2:r=20;break;default:r=99;}if(r==99)return 0;return 1;}' 0 'compiled OK' ''
hrun t_sw3   'int main(){int x;int r;x=1;r=0;switch(x){case 1:r=10;case 2:r=r+1;default:r=r+100;}if(r==111)return 0;return 1;}' 0 'compiled OK' ''
hrun t_swneg 'int main(){int x;int r;x=-1;r=0;switch(x){case -1:r=55;break;default:r=0;}if(r==55)return 0;return 1;}' 0 'compiled OK' ''
hrun t_swnest 'int main(){int x;int y;int r;x=1;y=2;r=0;switch(x){case 1:switch(y){case 2:r=42;break;default:r=7;}break;default:r=9;}if(r==42)return 0;return 1;}' 0 'compiled OK' ''
hrun t_swloop 'int main(){int i;int r;i=0;r=0;switch(3){case 3:while(i<5){i=i+1;if(i==2)break;}r=i;break;default:r=0;}if(r==2)return 0;return 1;}' 0 'compiled OK' ''
hrun t_sw_oob 'int main(){int x;x=1;case 3:return x;}' 1 'cc500: error' 'compiled OK'
# M12 编码锁定：switch 派发链用 cmp $imm,%eax（3d）逐 case。直观程序仅含赋值+switch——
# 若派发缺失/退化成串 if 则不必带 3d（条件比较用 39 c3），故 3d 出现 = 唯一派发标记（症状对立）。
if objdump -D -b binary -m i386 "$VD/t_sw1.elf" 2>/dev/null | grep -q '3d '; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 switch 派发 cmp \$imm,%eax (3d) 编码确认"
else
    echo "[FAIL] 宿主 switch 未检出 3d 派发（疑似派发缺失/退化成 if 链）"; HOST_FAIL=$((HOST_FAIL+1))
fi

echo "== [3/4] guest：ccboot 自举不动点 + < 运行语义 =="
if command -v qemu-system-i386 >/dev/null 2>&1; then
    if ! make BUILD="$BUILD" >/dev/null 2>&1; then echo "[FAIL] 内核构建失败"; exit 1; fi
    export DH_CC500_GUEST=1
    LOG="$BUILD/cc500_guest.log"; TIN="$BUILD/cc500_in.fifo"; TOUT="$BUILD/cc500_out.fifo"
    QPID=""; CAT_PID=""; GFAIL=0
    cleanup() { exec 9>&- 2>/dev/null || true; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true; rm -f "$TIN" "$TOUT"; }
    trap cleanup EXIT
    rm -f "$LOG" "$TIN" "$TOUT"; mkfifo "$TIN" "$TOUT"
    (cat "$TOUT" > "$LOG") & CAT_PID=$!
    qemu-system-i386 -kernel "$BUILD/kernel.elf" -display none -vga std -no-reboot -no-shutdown \
        -m 64 -nic none -serial stdio -monitor none < "$TIN" > "$TOUT" 2>/dev/null &
    QPID=$!; exec 9>"$TIN"
    # v1.1 收尾：默认等待窗 25s->60s；失败时自动转储 guest 串口尾部（慢 runner 自诊断，
    # 2026-09-01 CI 33504825917：2 vCPU 无 KVM 下 "guest < 运行 exit0" 20s 窗超时红）
    gwait() { local desc="$1" re="$2" tmo="${3:-60}" i; for ((i=0;i<tmo*4;i++)); do grep -aq "$re" "$LOG" 2>/dev/null && { echo "[ok]   $desc"; return 0; }; sleep 0.25; done; echo "[FAIL] $desc (缺: $re)"; GFAIL=$((GFAIL+1)); echo "      --- guest 串口尾部（自诊断） ---"; tail -n 8 "$LOG" 2>/dev/null | sed 's/^/      serial| /'; return 1; }
    gsend() { printf '%s\n' "$1" >&9; sleep 0.3; }
    gwait "shell 提示符" "mini-os\$ " 25
    # ccboot 自举：cc500 编译自身 P1==P2 逐字节一致（codegen 任何破坏当场暴露）
    gsend "ccboot"
    gwait "自举不动点 PASS" "\[ccboot\] byte-identical PASS" 60
    # 关系运算 < 运行语义：while(i<1) 循环恰 1 次 i==1 -> return 0（源码 <128B 避开 F-6 行截断）
    # 若 < 缺失则 parse fail（ccrun FAIL）；若 < 方向错则 i!=1 返回 1（ccrun FAIL）
    gsend 'writefile /tlt.c int main(){int i;i=0;while(i<1){i=i+1;}if(i==1)return 0;return 1;}'
    gsend "ccrun /tlt.c /tlt.elf"
    gwait "guest < 编译" "cc500: compiled OK" 60
    gwait "guest < 运行 exit0" "'/tlt.elf' exited code=0 PASS" 90
    # 变量/四则/== 语义（整机真值）：宿主无法运行 cc500 产物（qemu-i386 无内核给不了
    # 真实退出码），须靠整机 ccboot 内核 ccrun。注意 ccrun 仅认 code==0 为 PASS（shell.c
    # `code==0 ? PASS : FAIL`），故验证"a=1+2 后 a==3"要用内部 if 分支决定 return 0/1：
    # 加法/变量/== 任一错 -> return 1 -> code=1 FAIL。这也顺带覆盖 ==（已有 t_lt 覆盖 < ）。
    gsend 'writefile /sv.c int main(){int a;a=1+2;if(a==3)return 0;return 1;}'
    gsend "ccrun /sv.c /sv.elf"
    gwait "guest 变量四则编译" "cc500: compiled OK" 60
    gwait "guest a=1+2==3 return 0" "'/sv.elf' exited code=0 PASS" 90
    # M1：for 语义——for(i=0;i<3;i=i+1){s=s+2;} 恰 3 轮 -> s==6（step 后置/双跳摆渡布局错即 FAIL）
    gsend 'writefile /tfor.c int main(){int i;int s;s=0;for(i=0;i<3;i=i+1){s=s+2;}if(s==6)return 0;return 1;}'
    gsend "ccrun /tfor.c /tfor.elf"
    gwait "guest M1 for 编译" "cc500: compiled OK" 60
    gwait "guest M1 for s==6 exit0" "'/tfor.elf' exited code=0 PASS" 90
    # M1：do-while 语义——先执行一次再判，i 从 0 自增到 3 -> i==3
    gsend 'writefile /tdo.c int main(){int i;i=0;do{i=i+1;}while(i<3);if(i==3)return 0;return 1;}'
    gsend "ccrun /tdo.c /tdo.elf"
    gwait "guest M1 do 编译" "cc500: compiled OK" 60
    gwait "guest M1 do i==3 exit0" "'/tdo.elf' exited code=0 PASS" 90
    # M2：一元 - 语义——x=-x 应为 -5（一元/二元 '-' token 消歧错即 FAIL）
    gsend 'writefile /tneg.c int main(){int x;x=5;x=-x;if(x==0-5)return 0;return 1;}'
    gsend "ccrun /tneg.c /tneg.elf"
    gwait "guest M2 一元- 编译" "cc500: compiled OK" 60
    gwait "guest M2 x==-5 exit0" "'/tneg.elf' exited code=0 PASS" 90
    # M2：逻辑非语义——!0 == 1
    gsend 'writefile /tnot.c int main(){int x;x=0;if(!x)return 0;return 1;}'
    gsend "ccrun /tnot.c /tnot.elf"
    gwait "guest M2 !x 编译" "cc500: compiled OK" 60
    gwait "guest M2 !0==1 exit0" "'/tnot.elf' exited code=0 PASS" 90
    # M2：^ 与 ~ 语义——5^3==6 且 ~6 补码一致
    gsend 'writefile /tbit.c int main(){int x;x=5;x=x^3;if(x==6)if(~x==~6)return 0;return 1;}'
    gsend "ccrun /tbit.c /tbit.elf"
    gwait "guest M2 ^/~ 编译" "cc500: compiled OK" 60
    gwait "guest M2 ^/~ exit0" "'/tbit.elf' exited code=0 PASS" 90
    # M3：乘/除/模语义——7*6==42 且 17/5==3 且 17%5==2（C99 截断向零/余数随被除数）
    gsend 'writefile /tmul.c int main(){int x;int y;x=7*6;y=17/5;if(x==42)if(y==3)if(17%5==2)return 0;return 1;}'
    gsend "ccrun /tmul.c /tmul.elf"
    gwait "guest M3 乘除模 编译" "cc500: compiled OK" 60
    gwait "guest M3 42/3/2 exit0" "'/tmul.elf' exited code=0 PASS" 90
    # M3：优先级语义——2+3*4==14（multiplicative 高于 additive）与 (2+3)*4==20
    gsend 'writefile /tpre.c int main(){int x;int y;x=2+3*4;y=(2+3)*4;if(x==14)if(y==20)return 0;return 1;}'
    gsend "ccrun /tpre.c /tpre.elf"
    gwait "guest M3 优先级 编译" "cc500: compiled OK" 60
    gwait "guest M3 14/20 exit0" "'/tpre.elf' exited code=0 PASS" 90
    # M4：复合赋值链式语义——x=5;x+=3;x-=1;x*=2;x%=5 -> 8-1=7,*2=14,%5=4
    gsend 'writefile /tca.c int main(){int x;x=5;x+=3;x-=1;x*=2;x%=5;if(x==4)return 0;return 1;}'
    gsend "ccrun /tca.c /tca.elf"
    gwait "guest M4 复合赋值 编译" "cc500: compiled OK" 60
    gwait "guest M4 链式==4 exit0" "'/tca.elf' exited code=0 PASS" 90
    # M4：char 下标 lvalue 复合赋值运行语义——s[0]+=1（'A'+1=='B'，走 compound_assign
    # 的 8-bit store mov %al 路径）+ 全局 g 在首函数前（读回 g==0 验证全局存取，并验证入口
    # call 不被全局存储偏移：be_start 按「stub 后即首函数」算 rel32，program() 首个函数体处
    # 重定位到 main 修复，否则入口跳 g 存储挂死）。main 必须最先定义（cc500 契约「入口 =
    # 源码第一个函数」，cc500.c:40）；源码 <128B 走 writefile 单行，避开 heredoc。
    gsend 'writefile /tcalf.c int g;int main(){char *s;s="A";g=0;s[0]+=1;if(g==0)if(s[0]==66)return 0;return 1;}'
    gsend "ccrun /tcalf.c /tcalf.elf"
    gwait "guest M4 char下标 编译" "cc500: compiled OK" 60
    gwait "guest M4 s[0]+=1 exit0" "'/tcalf.elf' exited code=0 PASS" 90
    # M5：for+break 运行语义——0..9 遇 i==5 断，求和 0+1+2+3+4==10（break 跳转错即 FAIL）。
    # 注：M5 源较长，/128B 单行 writefile 会截断导致 compile 错（此前误匹配累计"compiled OK"），
    # 故一律走 heredoc（shell_heredoc.h），并加"源写入"确认锚点规避累计 grep 误判。
    gsend "writefile <<M /tbrk.c"
    gsend "int main(){int i;int s;s=0;for(i=0;i<10;i=i+1){if(i==5)break;s=s+i;}if(s==10)return 0;return 1;}"
    gsend "M"
    gwait "M5 for-break 源写入" "\[writefile\] '/tbrk.c' wrote" 40
    gsend "ccrun /tbrk.c /tbrk.elf"
    gwait "guest M5 for-break s==10 exit0" "'/tbrk.elf' exited code=0 PASS" 90
    gsend "rm /tbrk.c"; gsend "rm /tbrk.elf"
    # M5：for+continue 运行语义——跳过奇数，偶数求和 0+2+4+6+8==20
    gsend "writefile <<M /tcnt.c"
    gsend "int main(){int i;int s;s=0;for(i=0;i<10;i=i+1){if(i%2)continue;s=s+i;}if(s==20)return 0;return 1;}"
    gsend "M"
    gwait "M5 for-continue 源写入" "\[writefile\] '/tcnt.c' wrote" 40
    gsend "ccrun /tcnt.c /tcnt.elf"
    gwait "guest M5 for-continue s==20 exit0" "'/tcnt.elf' exited code=0 PASS" 90
    gsend "rm /tcnt.c"; gsend "rm /tcnt.elf"
    # M5：do-while+break 运行语义——i 累到 i==5 断，求和 1+2+3+4==10
    gsend "writefile <<M /tdob.c"
    gsend "int main(){int i;int s;s=0;i=0;do{i=i+1;if(i==5)break;s=s+i;}while(1);if(s==10)return 0;return 1;}"
    gsend "M"
    gwait "M5 do-break 源写入" "\[writefile\] '/tdob.c' wrote" 40
    gsend "ccrun /tdob.c /tdob.elf"
    gwait "guest M5 do-break s==10 exit0" "'/tdob.elf' exited code=0 PASS" 90
    gsend "rm /tdob.c"; gsend "rm /tdob.elf"
    # M5：嵌套 for+continue 运行语义——内层 j==1 continue（外层 2 轮 × 内层命中 2 次 = 4）
    gsend "writefile <<M /tnest.c"
    gsend "int main(){int i;int j;int s;s=0;for(i=0;i<2;i=i+1)for(j=0;j<3;j=j+1){if(j==1)continue;s=s+1;}if(s==4)return 0;return 1;}"
    gsend "M"
    gwait "M5 嵌套 源写入" "\[writefile\] '/tnest.c' wrote" 40
    gsend "ccrun /tnest.c /tnest.elf"
    gwait "guest M5 嵌套 s==4 exit0" "'/tnest.elf' exited code=0 PASS" 90
    gsend "rm /tnest.c"; gsend "rm /tnest.elf"
    # M6：短路运行语义（症状对立——右操作数必须被跳过，否则 return 1 FAIL）。
    # 0&&f()：&& 短路 → f() 不执行 → g 保持 0 → return 0。若贪心按位 &(0&1) 则 f() 被调 g==1 → FAIL。
    # 注意：cc500 需对后定义函数先写原型 `int f();`；且 main 须为第一个函数体（cc500 契约「入口=首个函数」）。
    gsend "writefile <<M /tand.c"
    gsend "int f();int g;int main(){int x;x=0&&f();if(g==0)return 0;return 1;}int f(){g=g+1;return 1;}"
    gsend "M"
    gwait "M6 && 短路 源写入" "\[writefile\] '/tand.c' wrote" 40
    gsend "ccrun /tand.c /tand.elf"
    gwait "guest M6 0&&f() 短路 exit0" "'/tand.elf' exited code=0 PASS" 90
    gsend "rm /tand.c"; gsend "rm /tand.elf"
    # 1||f()：|| 短路 → f() 不执行 → g 保持 0。若贪心则 g==1 FAIL。
    gsend "writefile <<M /tor.c"
    gsend "int f();int g;int main(){int x;x=1||f();if(g==0)return 0;return 1;}int f(){g=g+1;return 0;}"
    gsend "M"
    gwait "M6 || 短路 源写入" "\[writefile\] '/tor.c' wrote" 40
    gsend "ccrun /tor.c /tor.elf"
    gwait "guest M6 1||f() 短路 exit0" "'/tor.elf' exited code=0 PASS" 90
    gsend "rm /tor.c"; gsend "rm /tor.elf"
    # M6：&& 求值语义——(a>0)&&(a<5) 为真 return 0；混合优先级 a>0&&a<5||a==9 亦真。
    gsend 'writefile /tvand.c int main(){int a;a=3;if(a>0&&a<5)return 0;return 1;}'
    gsend "ccrun /tvand.c /tvand.elf"
    gwait "guest M6 && 值语义 编译" "cc500: compiled OK" 60
    gwait "guest M6 && 值语义 exit0" "'/tvand.elf' exited code=0 PASS" 90
    gsend 'writefile /tvandor.c int main(){int a;a=3;if(a>0&&a<5||a==9)return 0;return 1;}'
    gsend "ccrun /tvandor.c /tvandor.elf"
    gwait "guest M6 &&|| 优先级 编译" "cc500: compiled OK" 60
    gwait "guest M6 &&|| 优先级 exit0" "'/tvandor.elf' exited code=0 PASS" 90
    # M7：?: 运行语义——真/假分支各取正确值；且分支互斥（0?A:B 只求值 B）。
    # 单行 writefile 对含 ? 内容偶现 FS create fail → 全走 heredoc + 源写入锚点（防累计 grep 误判）。
    # 真分支：a=2 时 a>1 真 → (a>1?5:6)==5 → return 0
    gsend "writefile <<M /tqt.c"
    gsend "int main(){int a;int b;a=2;b=(a>1?5:6);if(b==5)return 0;return 1;}"
    gsend "M"
    gwait "M7 真 源写入" "\[writefile\] '/tqt.c' wrote" 40
    gsend "ccrun /tqt.c /tqt.elf"
    gwait "guest M7 ?: 真分支 b==5 exit0" "'/tqt.elf' exited code=0 PASS" 90
    gsend "rm /tqt.c"; gsend "rm /tqt.elf"
    # 假分支：a=0 时 a>1 假 → (a>1?5:6)==6 → return 0
    gsend "writefile <<M /tqf.c"
    gsend "int main(){int a;int b;a=0;b=(a>1?5:6);if(b==6)return 0;return 1;}"
    gsend "M"
    gwait "M7 假 源写入" "\[writefile\] '/tqf.c' wrote" 40
    gsend "ccrun /tqf.c /tqf.elf"
    gwait "guest M7 ?: 假分支 b==6 exit0" "'/tqf.elf' exited code=0 PASS" 90
    gsend "rm /tqf.c"; gsend "rm /tqf.elf"
    # 分支互斥：0?(g=g+1):(g=g+2) 只求值假分支 → g==2。若两分支都求值则 g==3 FAIL。
    gsend "writefile <<M /tqb.c"
    gsend "int g;int main(){int a;a=(0?(g=g+1):(g=g+2));if(g==2)return 0;return 1;}"
    gsend "M"
    gwait "M7 互斥 源写入" "\[writefile\] '/tqb.c' wrote" 40
    gsend "ccrun /tqb.c /tqb.elf"
    gwait "guest M7 ?: 假分支仅执行 g==2 exit0" "'/tqb.elf' exited code=0 PASS" 90
    gsend "rm /tqb.c"; gsend "rm /tqb.elf"
    # 嵌套三目：1?(2?3:4):5 == 3
    gsend "writefile <<M /tqn.c"
    gsend "int main(){int a;a=(1?(2?3:4):5);if(a==3)return 0;return 1;}"
    gsend "M"
    gwait "M7 嵌套 源写入" "\[writefile\] '/tqn.c' wrote" 40
    gsend "ccrun /tqn.c /tqn.elf"
    gwait "guest M7 ?: 嵌套==3 exit0" "'/tqn.elf' exited code=0 PASS" 90
    gsend "rm /tqn.c"; gsend "rm /tqn.elf"
# M8：++/-- 运行语义——后缀值=旧值、前缀值=新值、for 步进可用 i++。
    # 后缀旧值：r=i++ → r==5 && i==6（若后缀误返回新值则 r==6 FAIL）
    gsend 'writefile /tinc.c int main(){int i;int r;i=5;r=i++;if(i==6&&r==5)return 0;return 1;}'
    gsend "ccrun /tinc.c /tinc.elf"
    gwait "guest M8 i++ 编译" "cc500: compiled OK" 60
    gwait "guest M8 i++ 后缀旧值 r==5 exit0" "'/tinc.elf' exited code=0 PASS" 90
    gsend "rm /tinc.c"; gsend "rm /tinc.elf"
    # 前缀新值：j=++i → i==6 && j==6
    gsend 'writefile /tpre.c int main(){int i;int j;i=5;j=++i;if(i==6&&j==6)return 0;return 1;}'
    gsend "ccrun /tpre.c /tpre.elf"
    gwait "guest M8 ++i 编译" "cc500: compiled OK" 60
    gwait "guest M8 ++i 前缀新值 j==6 exit0" "'/tpre.elf' exited code=0 PASS" 90
    gsend "rm /tpre.c"; gsend "rm /tpre.elf"
    # 前缀自减新值：j=--i → i==4 && j==4（前缀 -- 与 ++ 同路径；栈记账错位时
    # 后续局部寻址偏格/退出码 -1，本断言即红——与上方 ++i 构成前缀双形态运行护栏）
    gsend 'writefile /tprd.c int main(){int i;int j;i=5;j=--i;if(i==4&&j==4)return 0;return 1;}'
    gsend "ccrun /tprd.c /tprd.elf"
    gwait "guest M8 --i 编译" "cc500: compiled OK" 60
    gwait "guest M8 --i 前缀新值 j==4 exit0" "'/tprd.elf' exited code=0 PASS" 90
    gsend "rm /tprd.c"; gsend "rm /tprd.elf"
    # 后缀自减旧值：r=i-- → i==4 && r==5
    gsend 'writefile /tded.c int main(){int i;int r;i=5;r=i--;if(i==4&&r==5)return 0;return 1;}'
    gsend "ccrun /tded.c /tded.elf"
    gwait "guest M8 i-- 编译" "cc500: compiled OK" 60
    gwait "guest M8 i-- 后缀旧值 r==5 exit0" "'/tded.elf' exited code=0 PASS" 90
    gsend "rm /tded.c"; gsend "rm /tded.elf"
    # for 步进用 i++：for(i=0;i<5;i++){s=s+i;} -> s==10
    gsend 'writefile /tforinc.c int main(){int i;int s;s=0;for(i=0;i<5;i++){s=s+i;}if(s==10)return 0;return 1;}'
    gsend "ccrun /tforinc.c /tforinc.elf"
    gwait "guest M8 for+i++ 编译" "cc500: compiled OK" 60
    gwait "guest M8 for i++ s==10 exit0" "'/tforinc.elf' exited code=0 PASS" 90
    gsend "rm /tforinc.c"; gsend "rm /tforinc.elf"
# M9：hex 运行语义——0xff==255、0xa==10、0x10+0x20==0x30(48)、大值 0xffffffff==-1（int 32 位）。
    # 任一 hex 解析错（如高 4 位丢弃/按十进制）即 return 1 -> code=1 FAIL（症状对立）。
    gsend "writefile <<M /thex.c"
    gsend "int main(){int a;int b;a=0xff;b=0xa;if(a==255)if(b==10)if(0x10+0x20==48)if(0xffffffff==-1)return 0;return 1;}"
    gsend "M"
    gwait "M9 hex 源写入" "\[writefile\] '/thex.c' wrote" 40
    gsend "ccrun /thex.c /thex.elf"
    gwait "guest M9 hex 编译" "cc500: compiled OK" 60
    gwait "guest M9 hex 语义 exit0" "'/thex.elf' exited code=0 PASS" 90
    gsend "rm /thex.c"; gsend "rm /thex.elf"
    # M9b：复合赋值补齐运行语义——/= 同 idiv 商、<<=/>>= 移位数据序(左旧值右移量、右操作数
    # 先入ecx)、位逻辑复合 |= &= ^=。任一错（如 '/' '=' 被拆开、移位对调）即 return 1 FAIL。
    gsend "writefile <<M /tcab.c"
    gsend "int main(){int x;int y;x=17;x/=5;y=1;y<<=4;y>>=2;if(x==3)if(y==4){x=0;x|=3;x&=5;x^=6;if(x==7)return 0;}return 1;}"
    gsend "M"
    gwait "M9b 复合赋值 源写入" "\[writefile\] '/tcab.c' wrote" 40
    gsend "ccrun /tcab.c /tcab.elf"
    gwait "guest M9b 复合赋值 编译" "cc500: compiled OK" 60
    gwait "guest M9b 复合赋值 语义 exit0" "'/tcab.elf' exited code=0 PASS" 90
    gsend "rm /tcab.c"; gsend "rm /tcab.elf"
# M11：goto/labels 运行语义（症状对立：任一前向回填/后向判据错即 return 1 FAIL）。
    # 前向 goto 跳过 a=99：若回填未解/a=99 被错误执行则 a!=7 FAIL。
    gsend "writefile <<M /tg1.c"
    gsend "int main(){int a;a=7;goto skip;a=99;skip:if(a==7)return 0;return 1;}"
    gsend "M"
    gwait "M11 前向goto 源写入" "\[writefile\] '/tg1.c' wrote" 40
    gsend "ccrun /tg1.c /tg1.elf"
    gwait "guest M11 前向goto 编译" "cc500: compiled OK" 60
    gwait "guest M11 前向goto 跳过a=99 exit0" "'/tg1.elf' exited code=0 PASS" 90
    gsend "rm /tg1.c"; gsend "rm /tg1.elf"
    # 后向 goto 构成循环到 i==5：若后向跳错则死循环/计数错 FAIL。
    gsend "writefile <<M /tg2.c"
    gsend "int main(){int i;i=0;top:i=i+1;if(i<5)goto top;if(i==5)return 0;return 1;}"
    gsend "M"
    gwait "M11 后向goto 源写入" "\[writefile\] '/tg2.c' wrote" 40
    gsend "ccrun /tg2.c /tg2.elf"
    gwait "guest M11 后向goto 编译" "cc500: compiled OK" 60
    gwait "guest M11 后向goto 循环 i==5 exit0" "'/tg2.elf' exited code=0 PASS" 90
    gsend "rm /tg2.c"; gsend "rm /tg2.elf"
    # 链式多标签依次跳过：s 保持 0 即证明三个前向 goto 全部正确跳断 s=1/2/3。
    gsend "writefile <<M /tg3.c"
    gsend "int main(){int a;int s;s=0;goto m0;s=1;m0:goto m1;s=2;m1:goto m2;s=3;m2:if(s==0)return 0;return 1;}"
    gsend "M"
    gwait "M11 链式goto 源写入" "\[writefile\] '/tg3.c' wrote" 40
    gsend "ccrun /tg3.c /tg3.elf"
    gwait "guest M11 链式goto 编译" "cc500: compiled OK" 60
    gwait "guest M11 链式goto s==0 exit0" "'/tg3.elf' exited code=0 PASS" 90
    gsend "rm /tg3.c"; gsend "rm /tg3.elf"
# M12：switch 运行语义（症状对立：派发错/贯通用错/break 命中错层即 return 1 FAIL）。
    # 命中 case2：r==20。
    gsend "writefile <<M /tsw1.c"
    gsend "int main(){int x;int r;x=2;r=0;switch(x){case 1:r=10;break;case 2:r=20;break;default:r=99;}if(r==20)return 0;return 1;}"
    gsend "M"
    gwait "M12 switch命中 源写入" "\[writefile\] '/tsw1.c' wrote" 40
    gsend "ccrun /tsw1.c /tsw1.elf"
    gwait "guest M12 switch命中 编译" "cc500: compiled OK" 60
    gwait "guest M12 switch命中 r==20 exit0" "'/tsw1.elf' exited code=0 PASS" 90
    gsend "rm /tsw1.c"; gsend "rm /tsw1.elf"
    # 默认分支：未中且无匹配 case → default r==99。
    gsend "writefile <<M /tsw2.c"
    gsend "int main(){int x;int r;x=7;r=0;switch(x){case 1:r=10;break;case 2:r=20;break;default:r=99;}if(r==99)return 0;return 1;}"
    gsend "M"
    gwait "M12 switch默认 源写入" "\[writefile\] '/tsw2.c' wrote" 40
    gsend "ccrun /tsw2.c /tsw2.elf"
    gwait "guest M12 switch默认 编译" "cc500: compiled OK" 60
    gwait "guest M12 switch默认 r==99 exit0" "'/tsw2.elf' exited code=0 PASS" 90
    gsend "rm /tsw2.c"; gsend "rm /tsw2.elf"
    # 贯通 fall-through：case1→case2→default 依次执行 r=10+1+100==111。
    gsend "writefile <<M /tsw3.c"
    gsend "int main(){int x;int r;x=1;r=0;switch(x){case 1:r=10;case 2:r=r+1;default:r=r+100;}if(r==111)return 0;return 1;}"
    gsend "M"
    gwait "M12 switch贯通 源写入" "\[writefile\] '/tsw3.c' wrote" 40
    gsend "ccrun /tsw3.c /tsw3.elf"
    gwait "guest M12 switch贯通 编译" "cc500: compiled OK" 60
    gwait "guest M12 switch贯通 r==111 exit0" "'/tsw3.elf' exited code=0 PASS" 90
    gsend "rm /tsw3.c"; gsend "rm /tsw3.elf"
    # 嵌套 switch：内层 break 只退内层、r==42。若 break 命中错误层级则 FAIL。
    # 注意：readline 行缓冲上限 128B（KB_LINE_MAX），heredoc 每行仍受此限——
    # 单行 >128B 会被静默截断（曾致 tswn.c 146B 截到 128B -> cc500 error）。故拆多行。
    gsend "writefile <<M /tswn.c"
    gsend "int main(){int x;int y;int r;x=1;y=2;r=0;"
    gsend "switch(x){case 1:switch(y){case 2:r=42;break;default:r=7;}break;default:r=9;}"
    gsend "if(r==42)return 0;return 1;}"
    gsend "M"
    gwait "M12 switch嵌套 源写入" "\[writefile\] '/tswn.c' wrote" 40
    gsend "ccrun /tswn.c /tswn.elf"
    gwait "guest M12 switch嵌套 编译" "cc500: compiled OK" 60
    gwait "guest M12 switch嵌套 r==42 exit0" "'/tswn.elf' exited code=0 PASS" 90
    gsend "rm /tswn.c"; gsend "rm /tswn.elf"
    # switch 内循环 break：内层 while break 退循环不退 switch、r==i==2。
    # 同 tswn：单行源码 130B > 128B 上限会被截断，拆多行。
    gsend "writefile <<M /tswl.c"
    gsend "int main(){int i;int r;i=0;r=0;"
    gsend "switch(3){case 3:while(i<5){i=i+1;if(i==2)break;}r=i;break;default:r=0;}"
    gsend "if(r==2)return 0;return 1;}"
    gsend "M"
    gwait "M12 switch内循环break 源写入" "\[writefile\] '/tswl.c' wrote" 40
    gsend "ccrun /tswl.c /tswl.elf"
    gwait "guest M12 switch内循环break 编译" "cc500: compiled OK" 60
    gwait "guest M12 switch内循环break r==2 exit0" "'/tswl.elf' exited code=0 PASS" 90
    gsend "rm /tswl.c"; gsend "rm /tswl.elf"
    if [ "$GFAIL" -gt 0 ]; then echo "[FAIL] guest 层 ${GFAIL} 项未过"; exit 1; fi
    echo "      guest 自举 + < 语义通过"
else
    echo "[warn] 无 qemu-system-i386，跳过 guest 层（宿主层已覆盖缺陷回归）"
fi

echo
echo "== [4/4] 汇总 =="
echo "宿主: PASS=$HOST_PASS FAIL=$HOST_FAIL"
if [ "$HOST_FAIL" -gt 0 ]; then echo "[FAIL] test_cc500 宿主层未全绿"; exit 1; fi
echo "[PASS] test_cc500 全绿"
exit 0