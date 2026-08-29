#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/run_host_tests.sh
# 宿主单元测试：把内核"纯逻辑"编译成普通 Linux 程序运行验证。
# 优点：不依赖 QEMU，秒级反馈，可用 ASan/valgrind 等宿主工具。
set -u
cd "$(dirname "$0")/.." || exit 1

CC="${CC:-gcc}"
# -no-pie：内核用 32 位地址当指针，宿主编译需把静态数据放在 4GB 内
# v0.19：源文件按子系统分目录（arch/kernel/mm/drv/fs/net/app），头文件逐目录 -I
CFLAGS="-Wall -Wextra -O1 -g -fno-pie -no-pie \
        -Isrc -Isrc/arch -Isrc/kernel -Isrc/mm -Isrc/drv -Isrc/fs -Isrc/net -Isrc/app -Itests \
        -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast"
BUILD="build"
PASS=0
FAIL=0

mkdir -p "$BUILD"

run_test() {
    local name="$1"; shift
    local srcs="$1"; shift
    if $CC $CFLAGS $srcs -o "$BUILD/$name" "$@"; then
        if "$BUILD/$name"; then
            echo "[OK]   $name"
            PASS=$((PASS + 1))
        else
            echo "[FAIL] $name（断言未通过）"
            FAIL=$((FAIL + 1))
        fi
    else
        echo "[FAIL] $name（编译失败）"
        FAIL=$((FAIL + 1))
    fi
}

# 每个测试： 套件名, 源文件（内核源码 + 测试源码；v0.19 起按子系统分目录）
run_test test_heap "src/mm/heap.c tests/test_heap.c"
run_test test_kb   "src/drv/kb.c tests/test_kb.c"
run_test test_sched "src/kernel/sched_policy.c tests/test_sched.c"
run_test test_sem   "src/kernel/sem.c tests/test_sem.c"
run_test test_msg   "src/kernel/msg.c tests/test_msg.c"
run_test test_fs    "src/fs/blockdev.c src/fs/fs.c tests/test_fs.c"
run_test test_elf   "src/kernel/elf.c tests/test_elf.c"
run_test test_guard "src/kernel/guard.c tests/test_guard.c"
run_test test_brk   "src/mm/brk.c tests/test_brk.c"
run_test test_userptr "src/kernel/userptr.c tests/test_userptr.c"
run_test test_netutil "src/net/netutil.c tests/test_netutil.c"
run_test test_ip      "src/net/ip.c tests/test_ip.c"
run_test test_udp     "src/net/ip.c src/net/udp.c tests/test_udp.c"
run_test test_icmp    "src/net/icmp.c src/net/ip.c src/net/udp.c tests/test_icmp.c"
run_test test_dhcp    "src/net/ip.c src/net/udp.c src/net/dhcp.c tests/test_dhcp.c"

echo
echo "宿主测试汇总: pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
