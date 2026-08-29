#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/run_host_tests.sh
# 宿主单元测试：把内核"纯逻辑"编译成普通 Linux 程序运行验证。
# 优点：不依赖 QEMU，秒级反馈，可用 ASan/valgrind 等宿主工具。
set -u
cd "$(dirname "$0")/.." || exit 1

CC="${CC:-gcc}"
# -no-pie：内核用 32 位地址当指针，宿主编译需把静态数据放在 4GB 内
CFLAGS="-Wall -Wextra -O1 -g -fno-pie -no-pie -Isrc -Itests \
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

# 每个测试： 套件名, 源文件（内核源码 + 测试源码）
run_test test_heap "src/heap.c tests/test_heap.c"
run_test test_kb   "src/kb.c tests/test_kb.c"
run_test test_sched "src/sched_policy.c tests/test_sched.c"
run_test test_sem   "src/sem.c tests/test_sem.c"
run_test test_msg   "src/msg.c tests/test_msg.c"
run_test test_fs    "src/blockdev.c src/fs.c tests/test_fs.c"
run_test test_elf   "src/elf.c tests/test_elf.c"

echo
echo "宿主测试汇总: pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
