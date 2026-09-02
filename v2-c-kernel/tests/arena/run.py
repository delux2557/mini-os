#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/arena/run.py
# agent 演练场 · 阶段0（task 契约 + 可复用 gate 判据）
#
# 从 record/replay 的 transcript 目录 .out.tr 构造"run 判定数据"，
# 供 task.py 裁判器直接对真实产出判分（无需把转录人肉翻译成 JSON）。
#
# 用法：
#   python3 tests/arena/run.py <transcript_dir> --json   # 输出 run 判定数据 JSON
#   （--json 默认开，exit 0）
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate import contract_hash, contract_lines  # noqa: E402


def build_run(dirpath):
    """从 transcript 目录构造 run 判定数据 dict。"""
    d = Path(dirpath)
    out_tr = d / "out.tr"
    lines = []
    out_bytes = 0
    if out_tr.exists():
        raw = out_tr.read_bytes()
        out_bytes = len(raw)
        lines = raw.decode("utf-8", errors="replace").split("\n")
        if lines and lines[-1] == "":
            lines.pop()
    cl = contract_lines(lines)
    return {
        "runid": d.name,
        "contract_hash": contract_hash(lines),
        "contract_lines": cl,  # 供 contract_content gate
        "out_lines": len(lines),
        "out_bytes": out_bytes,
    }


def main() -> int:
    p = argparse.ArgumentParser(description="从 transcript 目录构造 run 判定数据")
    p.add_argument("dir", help="transcript 目录（含 out.tr）")
    p.add_argument("--json", action="store_true", default=True, help="输出 JSON（默认）")
    args = p.parse_args()
    run = build_run(args.dir)
    print(json.dumps(run, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())