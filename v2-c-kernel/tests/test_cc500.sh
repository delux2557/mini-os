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

CC500=tools/cc500/cc500.c
CRT=tools/cc500/host_crt.c
VD=build/cc500
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
    out=$("${RUN[@]}" "$VD/$name.c" "$VD/$name.elf" 2>&1); rc=$?
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
# F-1：关系 < / > / >= / <= 均须能编译通过（编码与语义下方另行实证）
hrun t_lt 'int main(){int i;i=0;while(i<3){i=i+1;}return 0;}' 0 'compiled OK' ''
hrun t_gt 'int main(){int i;i=9;while(i>3){i=i-1;}return 0;}' 0 'compiled OK' ''
hrun t_ge 'int main(){int i;i=3;while(i>=3){i=i-1;}return 0;}' 0 'compiled OK' ''
hrun t_le 'int main(){int i;i=0;while(i<=3){i=i+1;}return 0;}' 0 'compiled OK' ''
# F-1：新增关系运算机器码编码锁定（setl=0f 9c，确认操作数序 != 照抄）—— 若符号缺失/错编码则该断言红
LT_PAT='0f 9c'
if objdump -D -b binary -m i386 "$VD/t_lt.elf" 2>/dev/null | grep -q "$LT_PAT"; then
    HOST_PASS=$((HOST_PASS+1)); echo "[ok]   宿主 < 编码确认 $LT_PAT (setl)"
else
    echo "[FAIL] 宿主 < 编码未检出 $LT_PAT"; HOST_FAIL=$((HOST_FAIL+1))
fi

echo "== [3/4] guest：ccboot 自举不动点 + < 运行语义 =="
if command -v qemu-system-i386 >/dev/null 2>&1; then
    if ! make >/dev/null 2>&1; then echo "[FAIL] 内核构建失败"; exit 1; fi
    export DH_CC500_GUEST=1
    LOG="build/cc500_guest.log"; TIN="build/cc500_in.fifo"; TOUT="build/cc500_out.fifo"
    QPID=""; CAT_PID=""; GFAIL=0
    cleanup() { exec 9>&- 2>/dev/null || true; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null || true; rm -f "$TIN" "$TOUT"; }
    trap cleanup EXIT
    rm -f "$LOG" "$TIN" "$TOUT"; mkfifo "$TIN" "$TOUT"
    (cat "$TOUT" > "$LOG") & CAT_PID=$!
    qemu-system-i386 -kernel build/kernel.elf -display none -vga std -no-reboot -no-shutdown \
        -m 64 -serial stdio -monitor none < "$TIN" > "$TOUT" 2>/dev/null &
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