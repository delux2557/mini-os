#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/split_line_grep.py
# 「整行 grep 的复核器」：处理**串口行非原子**导致的假红。
#
# 背景（实证，非推测）：guest 串口逐字符 `outb`，**用户进程**打一行时可能被抢占，于是这一行被
# **别的进程/内核心跳的输出插入而切成两段**（插入内容粘在前半片的**后面**）。CI 上实测到：
#     [shell] '[sched] wake pid=5 at tick=536   ← 前半片 `[shell] '` + 别人的整行粘在后面
#     [sched] wake pid=11 at tick=536
#     [sched] reap pid=3 name=deep code=0
#     [net] recvfrom sock=1 -> 0B
#     [sched] sleep pid=5 1 ticks (wake@537)
#     [sched] sleep pid=11 1 ticks (wake@537)
#     deep' exited code=0                        ← 后半片（另一行的行首）
# 此时 `grep -aq "'deep' exited code=0"` **永远匹配不上**，而 guest 侧一切正常
# （`[sched] exit pid=3 name=deep code=0` 也在日志里）⇒ 判据假红，与代码无关。
#
# 口径（**不放松判据**，针对**字面量**模式）：
#   目标行 = pattern[:p] + pattern[p:]（两片都非空），且
#     · pattern[:p] 出现在第 i 行的**末尾区**（其后粘着 ≤JUNK_MAX 字节的他人内容）
#     · pattern[p:] 是第 j 行（i < j ≤ i+span）的**行首**
#   只有"那一行确实被打印过"才可能命中 ⇒ 真正的缺失仍然不命中（自见 test_serial.sh 的
#   `split_matcher_selfcheck`：正例中、完整行不中、不存在的不中）。
#
# `.*`（仅 -F 模式）：把模式按 `.*` 切成若干**字面量段**，各段按顺序、在 span 行窗口内分别命中
#   即算命中。动机：`\[deepfork\] CHILD pid=.* grew beyond inherited stack` 这类模式里 `.*` 只吃
#   "pid=数字"，但一旦整行被切碎，正则的整行语义就永远不中；拆成两段字面量后各段可各自做上面的
#   两片复核，判据强度不变（两段都必须在）。
#
# 用法：split_line_grep.py [-F|--literal] <log> <模式> [span=12]
#   默认：仅接受**纯字面量**模式（含正则元字符者一律返回 1，行为与"整行 grep"一致）。
#   -F/--literal：把模式整体当**字面量**处理（跳过元字符否决），并把 `.*` 当**有序通配段分隔**。
#     调用方（tests/qemu_regression.sh）会先把 grep-BRE 里的 `\x` 转义去壳成裸字符
#     （`\[shell\]` → `[shell]`）再传入；对本来就是正则的（如 `[0-9][0-9]*`）去壳后是其字面写法、
#     日志里不会出现 ⇒ 仍不中，**不会假绿**。
#   退出码 0=命中（分片/分段拼接匹配） / 1=未命中 / 2=用法错
import sys

META = set("\\^$.*+?()[]{}|")
WILD = ".*"
JUNK_MAX = 200       # 后半片之前粘着的他人内容上限（一行别人的输出量级）
MIN_SEG = 6          # 分段复核时每段的最小长度（短段按序拼接易假绿 ⇒ 宁可不复核）


def lcp(a, b):
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return i


def find_segment(lines, seg, start, span):
    """在 lines[start:] 中找字面量 seg（允许被并发输出切成两片）。命中返回**末行下标**，否则 None。"""
    n = len(lines)
    for i in range(start, n):                 # 完整出现在某一行（调用方 grep 已失败时通常走不到，兜底用）
        if seg in lines[i]:
            return i
    head_ch = seg[0]
    last_p = len(seg) - 1                     # 两片都必须非空
    for i in range(start, n):
        head = lines[i]
        pos = head.find(head_ch)              # 前半片只可能在 seg[0] 出现处结束
        while pos >= 0:
            top = min(lcp(head[pos:], seg), last_p)
            for p in range(1, top + 1):
                if len(head) - pos - p > JUNK_MAX:
                    continue                  # 后半片之后粘的内容过长 ⇒ 不是"同一行被切"
                tail = seg[p:]
                for j in range(i + 1, min(i + span + 1, n)):
                    if lines[j].startswith(tail):
                        return j
            pos = head.find(head_ch, pos + 1)
    return None


def main():
    argv = sys.argv[1:]
    literal = False
    if argv and argv[0] in ("-F", "--literal"):
        literal = True
        argv = argv[1:]
    if len(argv) < 2:
        print("usage: split_line_grep.py [-F|--literal] <log> <pattern> [span]", file=sys.stderr)
        return 2
    path, pattern = argv[0], argv[1]
    span = int(argv[2]) if len(argv) > 2 else 12
    if not pattern:
        return 1
    if literal:
        segs = [s for s in pattern.split(WILD) if s]   # `.*` ⇒ 有序字面量段边界
        if len(segs) > 1 and any(len(s) < MIN_SEG for s in segs):
            return 1                                  # 段过短：有序拼接易假绿 ⇒ 不复核，回到严格判据
    else:
        if any(ch in META for ch in pattern):
            return 1
        segs = [pattern]
    if not segs:
        return 1
    try:
        with open(path, "rb") as fh:
            lines = fh.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return 1
    cur = 0
    for seg in segs:
        end = find_segment(lines, seg, cur, span)
        if end is None:
            return 1
        cur = end                              # 下一段从本段末行起（可能紧邻同一行）
    print("[split-line] 命中：%d 段按序匹配 ⇒ 目标行 '%s'" % (len(segs), pattern))
    return 0


if __name__ == "__main__":
    sys.exit(main())
