#!/usr/bin/env bash
# mini-os/v2-c-kernel/tests/transcript.sh
# record/replay 地基 · P2 录制内核（transcript 固化）
#
# source 到被测脚本，把"串口输入命令流"与"串口输出字节流"固化为可归档、可复现、
# 可逐步差分比对的 transcript。这是"录放雏形"的录制侧；回放/差分（P3）消费本产物。
#
# 产物约定（build/transcripts/<runid>/）：
#   .in.tr   输入流：每行 = 序号 \t 相对毫秒 \t 注入的串口命令/字节（可重放/审计）
#   .out.tr  输出流：串口从启动到收尾的完整原始字节（一轮的"黄金输出"）
#
# 用法：
#   . "$(dirname "$0")/transcript.sh"
#   tr_start serial-test            # 初始化转录（runid），前置 mkdir/fifo/QEMU 由调用方建
#   tr_send help                    # 注入命令并记录到 .in.tr
#   tr_snapshot $LOG                # 把当前串口日志存进 .out.tr（失败点冻结现场）
#   tr_finish $LOG                  # 收尾归档；返回 0=ok / 1=失败自动归档（调用方再回 0/1）
#   tr_abort <desc> $LOG            # 中途失败：立即归档 .in.tr+.out.tr，输出归档路径
#
# 环境变量：
#   TR_BASE  归档根目录（默认 build/transcripts）
#   TR_RUNID 本轮 runid（默认 <脚本名>-<时间戳>）
set -u

TR_BASE="${TR_BASE:-build/transcripts}"
TR_NOW="$(date +%s%3N)"                  # 相对毫秒游标（host 墙钟；真 icount 虚拟时钟留 P3）
TR_FIRST=""
TR_DIR=""

# 单条记录输入：序号 / 相对毫秒 / 注入字节
tr_emit_in() {
    local rel=0
    if [ -n "$TR_FIRST" ]; then rel=$(( $(date +%s%3N) - TR_FIRST )); fi
    printf '%d\t%d\t%s\n' "$1" "$rel" "$2" >> "$TR_DIR/in.tr"
}

# 初始化转录目录：建 runid 子目录，写 header，记首发时刻
tr_start() {
    local name="${1:-transcript}"
    local runid="${TR_RUNID:-${name}-$(date +%Y%m%d-%H%M%S)}"
    TR_DIR="$TR_BASE/$runid"
    mkdir -p "$TR_DIR"
    TR_FIRST="$(date +%s%3N)"
    {
        printf '# mini-os record/replay transcript\n'
        printf '# runid: %s\n' "$runid"
        printf '# cols:  seq \\t rel_ms \\t payload\n'
        printf '# 规约:  payload 禁原始 TAB/换行，须单行；多行需显式编码 \\t\\n\\\\ 并同步回放解码\n'
    } > "$TR_DIR/in.tr"
    : > "$TR_DIR/out.tr"
    echo "[transcript] 录制开始 -> $TR_DIR"
}

# 注入一条命令：echo 到 fd 9（串口）+ 记录进 in.tr（seq 自动递增）
# 防御（v1.4.6）：in.tr 是 TSV seq \t rel_ms \t payload，payload 若混入原始 TAB/换行会破坏列分隔，
# 使差分/回放静默异常。故在唯一写入钳制点做 fail-fast：违规即拒绝（不发、不记）并报明细——
# 让"坏 TSV 证据"结构上不可能产生。当前统一单行 writefile，无此输入；为防未来 heredoc 误入，
# 禁令写死在文件头规约里。若某天真需多行：显式编码 \t\n\\ + replay 同型解码后再放开此守卫。
tr_seq=0
tr_send() {
    if [[ "$1" == *$'\t'* || "$1" == *$'\n'* ]]; then
        echo "[transcript] 拒绝 payload：含 TAB/换行，会破坏 in.tr 的 TSV 列分隔。命令须单行。" >&2
        printf '  offending: %q\n' "$1" >&2
        echo "[transcript] 提示：当前规约=单行（如 writefile <path> <content>）。多行需显式编码后同步回放解码。" >&2
        return 1
    fi
    tr_seq=$((tr_seq + 1))
    printf '%s\n' "$1" >&9
    tr_emit_in "$tr_seq" "$1"
}

# 快照输出：把调用方串口日志当前内容拷入 out.tr（覆盖式，保留"到达失败点的字节")
tr_snapshot() {
    local log="${1:-build/serial_term.log}"
    cp "$log" "$TR_DIR/out.tr" 2>/dev/null || : > "$TR_DIR/out.tr"
}

# 归档最终 transcript：把 in/out 拷贝到 runid 目录已成（in.tr/out.tr 常驻故无需重复）。
# 真正要"固化"的是一次失败的可复现现场——这里写一个 README 标记成功/失败语义。
tr_finish() {
    local rc="${1:-0}"
    if [ "$rc" -eq 0 ]; then
        printf '# result: PASS\n' > "$TR_DIR/RESULT"
        echo "[transcript] 完成 (PASS): $TR_DIR (in=$(wc -l < "$TR_DIR/in.tr") 条输出=$(wc -c < "$TR_DIR/out.tr")B)"
    else
        printf '# result: FAIL\n' > "$TR_DIR/RESULT"
        echo "[transcript] 完成 (FAIL，已固化可复现现场): $TR_DIR"
    fi
}

# 中途失败：标注 FAIL 并输出归档路径（供调用方把 .in.tr/.out.tr 落到日志）
tr_abort() {
    local desc="$1" log="${2:-build/serial_term.log}"
    tr_snapshot "$log"
    printf '# result: FAIL (@ %s)\n' "$desc" > "$TR_DIR/RESULT"
    echo "[transcript] 失败归档: $TR_DIR"
    echo "              in.tr: $(wc -l < "$TR_DIR/in.tr") 条  out.tr: $(wc -c < "$TR_DIR/out.tr")B"
}