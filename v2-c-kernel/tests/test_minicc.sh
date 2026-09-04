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
# 坏数字：0x10 / 123abc -> 必须 FAIL + bad number
hrun t_hex 'int main(){return 0x10;}' 1 'bad number' 'compiled OK'
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
hrun t_ptrmism 'int main(){int x;int* p;p=5;return 0;}' 1 'type mismatch' 'compiled OK'
hrun t_ptrmism2 'int main(){int x;int* p;x=p;return 0;}' 1 'type mismatch' 'compiled OK'
echo "== [2b] 宿主成功路径 =="
# 成功：变量/四则/if/else/while/递归/全局/逻辑，编译层全过
hrun t_arith 'int main(){int a;a=1+2*3-4;return 0;}' 0 'compiled OK' ''
hrun t_rec 'int fact(int n){if(n<=1)return 1;return n*fact(n-1);}int main(){return 0;}' 0 'compiled OK' ''
hrun t_logic 'int main(){int a;a=1;if(a==1&&!(a==0)||0==1)return 0;return 1;}' 0 'compiled OK' ''
hrun t_global 'int g=7;int main(){int x;x=g;return 0;}' 0 'compiled OK' ''
# 指针（V2b）：取地址/解引用/指针参数/指针算术
hrun t_ptr 'int main(){int x;x=5;int* p;p=&x;if(*p==5)return 0;return 1;}' 0 'compiled OK' ''
hrun t_ptrwrite 'int main(){int a;int* p;p=&a;*p=7;if(a==7)return 0;return 1;}' 0 'compiled OK' ''
hrun t_ptrarg 'int f(int* p){return *p;}int main(){int a;a=3;if(f(&a)==3)return 0;return 1;}' 0 'compiled OK' ''
hrun t_ptrarith 'int main(){int a;a=10;int* p;p=&a;if(*(p+0)==10)return 0;return 1;}' 0 'compiled OK' ''

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
    # 递归 + 参数 + 乘法
    gsend "writefile /mf.c int fact(int n){if(n<=1)return 1;return n*fact(n-1);}int main(){if(fact(5)==120)return 0;return 1;}"
    gsend "micc /mf.c /mf.elf"
    gwait "fact(5)==120 编译" "minicc: compiled OK" 40
    gwait "fact(5)==120 运行" "\[micc\] '/mf.elf' exited code=0 PASS" 40
    # 全局 + while + 除法/取模 + &&（源码 <128B 避开 writefile 单行截断，同 test_cc500 F-6）
    gsend "writefile /mg.c int g;int main(){int i;i=0;while(i<10){g=g+i;i=i+1;}if(g==45&&17/5==3&&17%5==2)return 0;return 1;}"
    gsend "micc /mg.c /mg.elf"
    gwait "while+div/mod 编译" "minicc: compiled OK" 40
    gwait "g==45 && 17/5==3 && 17%5==2" "\[micc\] '/mg.elf' exited code=0 PASS" 40
    # 非零退出码语义：return 1 -> micc 必须报 FAIL（证明退出码真实传回，非恒 0）
    gsend "writefile /m1.c int main(){return 1;}"
    gsend "micc /m1.c /m1.elf"
    gwait "return 1 -> FAIL 语义" "\[micc\] '/m1.elf' exited code=1 FAIL" 40
    # 编译错误路径：未定义函数 -> compile FAIL（编译器自身 rc=1 传回 shell）
    gsend "writefile /me.c int main(){return g();}"
    gsend "micc /me.c /me.elf"
    gwait "undefined 编译报错" "\[micc\] compile FAIL" 40
    # 指针（V2b）：取地址/解引用读写/指针参数/指针算术/多级指针拒绝
    gsend "writefile /pa.c int main(){int x;x=5;int* p;p=&x;if(*p==5)return 0;return 1;}"
    gsend "micc /pa.c /pa.elf"
    gwait "指针取地址解引用" "\[micc\] '/pa.elf' exited code=0 PASS" 40
    gsend "writefile /pb.c int main(){int a;int* p;p=&a;*p=7;if(a==7)return 0;return 1;}"
    gsend "micc /pb.c /pb.elf"
    gwait "指针解引用写" "\[micc\] '/pb.elf' exited code=0 PASS" 40
    gsend "writefile /pc.c int f(int* p){return *p;}int main(){int a;a=3;if(f(&a)==3)return 0;return 1;}"
    gsend "micc /pc.c /pc.elf"
    gwait "指针参数 f(&a)" "\[micc\] '/pc.elf' exited code=0 PASS" 40
    gsend "writefile /pd.c int main(){int a;a=10;int* p;p=&a;if(*(p+0)==10)return 0;return 1;}"
    gsend "micc /pd.c /pd.elf"
    gwait "指针算术 p+0" "\[micc\] '/pd.elf' exited code=0 PASS" 40
    gsend "writefile /pe.c int main(){int* p;int** q;return 0;}"
    gsend "micc /pe.c /pe.elf"
    gwait "多级指针编译拒绝" "\[micc\] compile FAIL" 40
    if [ "$GFAIL" -gt 0 ]; then echo "[FAIL] guest 层 ${GFAIL} 项未过"; exit 1; fi
    echo "      guest micc 端到端通过"
else
    echo "[warn] 无 qemu-system-i386，跳过 guest 层（宿主层已覆盖错误路径与编码）"
fi

echo
echo "== [4/4] 汇总 =="
echo "宿主: PASS=$HOST_PASS FAIL=$HOST_FAIL"
if [ "$HOST_FAIL" -gt 0 ]; then echo "[FAIL] test_minicc 宿主层未全绿"; exit 1; fi
echo "[PASS] test_minicc 全绿"
exit 0
