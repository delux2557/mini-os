#!/usr/bin/env bash
# build_audit.sh — 构建审计环境：gcc -m32 freestanding（构建需 gcc-multilib 头〔与 test-cc500 同源〕，无需 32 位 libc：自带 crt/start；运行需 ia32 exec 或 qemu-i386，二者具备其一即可）
# 用法: CC500_SRC=<repo>/v2-c-kernel/tools/cc500 bash build_audit.sh [输出目录]
set -u
cd "$(dirname "$0")/.." || exit 1
SRC_DIR="${CC500_SRC:?需要 CC500_SRC=<path>/v2-c-kernel/tools/cc500（clone 的 mini-os 仓库目录）}"
F="-m32 -ffreestanding -fno-pie -no-pie -fno-stack-protector -O1 -std=gnu99 -fpermissive -w"
B=bin; mkdir -p $B

# ---- 陈旧检查模式（不构建）：`build_audit.sh --check-stale`，0=需（重）建 / 1=无需 ----
# 动机（陈旧产物陷阱，本会话实证）：调用方此前只判"产物二进制是否存在" ⇒ 改了 `cc500.c` /
# `minicc.c` / `harness_src/*` 后**不重编**，基线/台账仍跑**旧二进制**，把"代码已改"误报成
# "产物漂移"假红（切回 main 后 SPLIT-C-dup* 三项显示的是 F7 修复前的值）。改为按**依赖 mtime** 判定。
if [ "${1:-}" = "--check-stale" ]; then
    for o in hostcc500 hostminicc32; do [ -x "$B/$o" ] || exit 0; done
    newest=""
    for f in "$SRC_DIR/cc500.c" "${MINICC_SRC:-}/minicc.c" harness_src/*.c harness_src/*.s "$0"; do
        [ -e "$f" ] || continue
        if [ -z "$newest" ] || [ "$f" -nt "$newest" ]; then newest="$f"; fi
    done
    [ -n "$newest" ] || exit 1
    for o in hostcc500 hostminicc32; do [ "$newest" -nt "$B/$o" ] && exit 0; done
    exit 1
fi
[ -f "$SRC_DIR/cc500.c" ] || { echo "[ERR] 无 $SRC_DIR/cc500.c"; exit 2; }
gcc $F -c "$SRC_DIR/cc500.c" -o $B/cc500.o     || exit 2
gcc $F -c harness_src/crt500.c -o $B/crt500.o  || exit 2
gcc $F -c harness_src/cc500run.c -o $B/run.o   || exit 2
[ -f $B/start32.o ] && [ $B/start32.o -nt harness_src/start32.s ] || gcc -m32 -w -c harness_src/start32.s -o $B/start32.o || exit 2
gcc -m32 -static -nostdlib -no-pie -o $B/hostcc500  $B/cc500.o $B/crt500.o $B/start32.o || exit 2
gcc -m32 -static -nostdlib -no-pie -o $B/cc500run   $B/run.o $B/start32.o              || exit 2
echo "built: bin/hostcc500 bin/cc500run"

# ---- 补丁①（P0-1）：minicc 侧构建 hostminicc32/runmin32（同一 freestanding 套路，与 cc500 共用 start32.o）----
if [ -n "${MINICC_SRC:-}" ]; then
  gcc $F -c "$MINICC_SRC/minicc.c" -o $B/minicc.o  || exit 2
  gcc $F -c harness_src/crt32.c    -o $B/crt32.o   || exit 2
  gcc $F -c harness_src/runmin.c   -o $B/runmin.o  || exit 2
  gcc -m32 -static -nostdlib -no-pie -o $B/hostminicc32 $B/minicc.o $B/crt32.o $B/start32.o || exit 2
  gcc -m32 -static -nostdlib -no-pie -o $B/runmin32    $B/runmin.o  $B/start32.o            || exit 2
  echo "built: bin/hostminicc32 bin/runmin32"
fi
