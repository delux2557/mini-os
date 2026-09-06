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
# BUG-049：数字字面量混入字母 -> 必须 FAIL 且报出错 token，不得静默算错骗 compiled OK
hrun t_mixhex 'int main(){return 0x10;}' \
     1 '0x10' 'compiled OK'
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
# M4 边界：'/=' 不在 M4 范围，x/=2 须干净报错（'/' 走注释分支，按 '/' '=' 解析后语法错）
hrun t_ca_sdeq 'int main(){int x;x=5;x/=2;return x;}' 1 'cc500: error' 'compiled OK'
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