#!/usr/bin/env bash
# mini-os/v2-c-kernel/tools/minicc/diffsynth/gen_guest_elfs.sh
# 方案 B：生成 N 个随机编译源并用 minicc 批量编译成 guest .elf，再 objcopy 成可嵌内嵌的 _elf.o。
# 专供 `make GUEST_DIFF=1 test-diffsynth-guest` 使用——被试"编译器 = 宿主版 minicc"(hostminicc)，
# 产物即为 mini-os 里可 `run` 的 ELF；其语义退码由 guest run 拿取，与 gcc 参考差分。
#
# 产物命名 ds00..ds(N-1).c / .elf / _elf.o（storage.c 的 #ifdef GUEST_DIFF 用 _binary_dsXX_elf 引用）。
# 用法: gen_guest_elfs.sh <DIR> <GEN_BIN> [count]
set -u
DIR="${1:?DIR}"; GEN_BIN="${2:?GEN_BIN}"; count="${3:-12}"
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_MINI="$(dirname "$SELF_DIR")"                 # .../tools/minicc

mkdir -p "$DIR"
HOSTMINICC="$DIR/hostminicc"
if [ ! -x "$HOSTMINICC" ]; then
  gcc -m32 -std=gnu99 -O1 -w -fpermissive -o "$HOSTMINICC" \
      "$TOOLS_MINI/minicc.c" "$TOOLS_MINI/host_crt.c" || { echo "[ERR] hostminicc 构建失败"; exit 2; }
fi

# 1) 生成 count 个随机源（prog_001.c..，见 gen.c --out）
"$GEN_BIN" --seed 7 --target minicc --vars 4 --stmts 6 --count "$count" --out "$DIR" || { echo "[ERR] gen 失败"; exit 2; }

# 2) 重命名并逐个 minicc 编译成 dsNN.elf（hostminicc 是 32 位，宿主无 ia32 exec → qemu-i386）
for ((i=0;i<count;i++)); do
  three=$(printf '%03d' $((i+1)))
  dst=$(printf 'ds%02d' "$i")
  cp "$DIR/prog_${three}.c" "$DIR/${dst}__src.c"
  if ! { timeout 20 qemu-i386 "$HOSTMINICC" "$DIR/${dst}__src.c" "$DIR/${dst}.elf"; } 2>/dev/null; then
    echo "[WARN] minicc 拒样本 $dst（gcc 侧仍测 acceptance，此处占位空 .o 防链接崩；guest run 将显式 FAIL）"
    : > "$DIR/${dst}_elf.o"
    rm -f "$DIR/${dst}.elf"
    continue
  fi
  # 3) 原字节内嵌：objcopy binary -> elf32-i386，符号 _binary_dsNN_elf_start/end
  ( cd "$DIR" && objcopy -I binary -O elf32-i386 -B i386 "${dst}.elf" "${dst}_elf.o" )
done

# 供 make 判完成的哨兵
touch "$DIR/.done"
echo "[gen_guest_elfs] $count 样本编译/内嵌完成 -> $DIR"
exit 0