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
# 口径（**不放松判据**，针对**纯字面量**模式）：
#   目标行 = pattern[:p] + pattern[p:]（两片都非空），且
#     · pattern[:p] 出现在第 i 行的**末尾区**（其后粘着 ≤JUNK_MAX 字节的他人内容）
#     · pattern[p:] 是第 j 行（i < j ≤ i+span）的**行首**
#   只有"那一行确实被打印过"才可能命中 ⇒ 真正的缺失仍然不命中（自见 test_serial.sh 的
#   `split_matcher_selfcheck`：正例中、完整行不中、不存在的不中）。
# 含正则元字符的模式不做本复核（前缀语义不适用）⇒ 返回未命中，行为与改动前一致，不会假绿。
#
# 用法：split_line_grep.py <log> <字面量模式> [span=12]
#   退出码 0=命中（两片拼接匹配） / 1=未命中 / 2=用法错
import sys

META = set("\\^$.*+?()[]{}|")
JUNK_MAX = 200       # 后半片之前粘着的他人内容上限（一行别人的输出量级）


def lcp(a, b):
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return i


def main():
    if len(sys.argv) < 3:
        print("usage: split_line_grep.py <log> <literal-pattern> [span]", file=sys.stderr)
        return 2
    path, pattern = sys.argv[1], sys.argv[2]
    span = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    if not pattern or any(ch in META for ch in pattern):
        return 1
    try:
        with open(path, "rb") as fh:
            lines = fh.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return 1
    head_ch = pattern[0]
    last_p = len(pattern) - 1                 # 两片都必须非空
    for i, head in enumerate(lines):
        pos = head.find(head_ch)              # 前半片只可能在 pattern[0] 出现处结束
        while pos >= 0:
            top = min(lcp(head[pos:], pattern), last_p)
            for p in range(1, top + 1):
                if len(head) - pos - p > JUNK_MAX:
                    continue                  # 后半片之后粘的内容过长 ⇒ 不是"同一行被切"
                tail = pattern[p:]
                for j in range(i + 1, min(i + span + 1, len(lines))):
                    if lines[j].startswith(tail):
                        print("[split-line] 命中：行 %d 的 '%s' + 行 %d 的 '%s' ⇒ 拼接 == 目标行 '%s'"
                              % (i + 1, pattern[:p], j + 1, tail, pattern))
                        return 0
            pos = head.find(head_ch, pos + 1)
    return 1


if __name__ == "__main__":
    sys.exit(main())
