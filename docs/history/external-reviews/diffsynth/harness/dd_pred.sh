#!/usr/bin/env bash
# dd_pred.sh — ddmin.sh 的自定义谓词示例：运行语义差分（gcc 参考 vs minicc 产物）。
# 用法：DD_PROG=/path/to/dd_pred.sh DD_BUILD=/tmp/ddmin_build bash ddmin.sh <src.c>
# 依赖：gcc（native）+ 本包构建的 hostminicc32 / runmin32（无需 QEMU）。
f="$1"
D="$(mktemp -d)"
gcc -O0 -std=gnu89 -fno-builtin -w -o "$D/ref" "$f" 2>/dev/null || exit 1   # gcc 拒=好
"$D/ref" >/dev/null 2>&1; ref=$?
H="${HMINICC:-/tmp/ddmin_build/hostminicc}"
"$H" "$f" "$D/p.elf" >/dev/null 2>&1 || exit 1                              # minicc 拒=好（acceptance 谓词）
R="${RUNMIN:-/tmp/ddmin_build/runmin32}"
"$R" "$D/p.elf" >/dev/null 2>&1; rn=$?
[ "$((ref & 0xff))" != "$((rn & 0xff))" ] && exit 0                          # 语义差=坏（仍触发）
exit 1
