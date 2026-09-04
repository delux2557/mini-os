#!/usr/bin/env bash
# 加固 A-1 ③：从无符号表内核 ELF 抽函数符号，生成 C 符号表源码。
# 用法: gen_kernsym.sh <nosym.elf> <ksym_tab.c>
# 生成按地址升序的 `struct ksym { addr; name; } ksym_tab[]` + `ksym_count`，
# 供内核 ksym_name() 用二分查"某地址所属函数符号"（panic 回溯打印函数名）。
set -eu
ELF="$1"
OUT="$2"
if ! command -v nm >/dev/null 2>&1; then
    echo "[gen_kernsym] missing nm" >&2
    exit 2
fi
cnt=$(nm -n "$ELF" 2>/dev/null | awk '$2=="t"||$2=="T" { n++ } END { print n+0 }')
{
    echo "/* auto-generated kernel symbol table (by tools/gen_kernsym.sh) */"
    echo "struct ksym { unsigned int addr; const char *name; };"
    echo "const struct ksym ksym_tab[] = {"
    nm -n "$ELF" 2>/dev/null | awk '$2=="t"||$2=="T" { printf "    {0x%s, \"%s\"},\n", $1, $3 }'
    echo "};"
    echo "const unsigned int ksym_count = $cnt;"
} > "$OUT"