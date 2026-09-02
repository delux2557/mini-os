#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/arena/task.py
# agent 演练场 · 阶段0（task 契约 + 可复用 gate 判据）
#
# 任务契约 task.json：把"agent 要做什么、改完怎么判分"落成机器可读的规范。
# 一个 task 定义为一个题目（关卡），可被 agent 读取以理解目标与门禁，
# 也可被裁判器据此对"一轮 run 的产出"自动判分。
#
# task.json 结构（字段约定）：
# {
#   "id":        "arena-001-ccboot",        // 唯一 id
#   "title":     "ccboot 自举不动点",        // 人类可读标题
#   "layer":     "toolchain",              // 归属子系统（可选）
#   "difficulty": 2,                       // 难度 1-5（可选）
#   "prompt":    "在 guest 内跑 ccboot…",   // 给 agent 的目标描述
#   "base":      {"runid": "…"},           // 基线的判定数据来源（可选，见下）
#   "gates":     ["contract_drift", "output_lines_drift"],  // 命中的判据
#   "tolerance": 0.20,                     // 漂移容忍度（可选，默认 COUNT_TOL）
# }
#
# 用法：
#   python3 tests/arena/task.py <task.json> --run <transcript-dir> [--base <transcript-dir>]
#     --run  当轮 transcript 目录（经 run.py 构造判定数据）
#     --base 基线 transcript 目录（可选；结果一致则 PASS，漂移则 FAIL）
#     输出：每项 gate 的 (name, ok, msg) + 汇总判定 PASS/FAIL。
#   exit: 0=全过 PASS / 1=有判据 FAIL
import argparse
import json
import sys
from pathlib import Path

# 使本目录可被当包 import（与 baseline_check.py 复用同一 gate.py）
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate import (  # noqa: E402
    GATES,
    COUNT_TOL,
    run_gates,
)
from run import build_run  # noqa: E402


def load_task(task_path):
    """读取并校验 task.json；返回 dict。校验失败抛 ValueError。"""
    data = json.loads(Path(task_path).read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("task 顶层必须是对象")
    if not data.get("id"):
        raise ValueError("task 缺 'id'")
    gates = data.get("gates")
    if not isinstance(gates, list) or not gates:
        raise ValueError("task 缺非空 'gates' 列表")
    unknown = [g for g in gates if g not in GATES]
    if unknown:
        raise ValueError(f"task 引用了未注册 gate: {unknown}（可用: {sorted(GATES)}）")
    return data


def judge(task, run, base):
    """对当轮 run（对照 base）执行 task 的全部 gate；返回 (results, passed)。"""
    tol = task.get("tolerance", COUNT_TOL)
    results = run_gates(run, base, tol)
    # 只判 task.gates 里点名的判据
    wanted = set(task["gates"])
    results = [r for r in results if r["name"] in wanted]
    return results, all(r["ok"] for r in results)


def _print_results(results):
    for r in results:
        mark = "PASS" if r["ok"] else "FAIL"
        print(f"  [{mark}] {r['name']:<24} {r['msg']}")


def main() -> int:
    p = argparse.ArgumentParser(description="agent 演练场 · task 契约裁判")
    p.add_argument("task", help="task.json 路径")
    p.add_argument("--run", required=True, help="当轮 transcript 目录（含 out.tr）")
    p.add_argument("--base", help="基线 transcript 目录（可选；缺省用 task.base.runid 对应目录，再缺省视为无基线）")
    args = p.parse_args()

    try:
        task = load_task(args.task)
    except ValueError as e:
        print(f"[config error] {e}", file=sys.stderr)
        return 2

    run = build_run(args.run)
    base = None
    if args.base:
        base = build_run(args.base)
    elif task.get("base") and task["base"].get("runid"):
        # 约定：基线目录在 run 目录同层，名为 task.base.runid
        p = Path(args.run).parent / task["base"]["runid"]
        if p.exists():
            base = build_run(p)

    print(f"task: {task['id']} — {task.get('title', '')}")
    results, passed = judge(task, run, base)
    _print_results(results)
    print("==> " + ("PASS" if passed else "FAIL"))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())