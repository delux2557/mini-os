#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/arena/qw.py
# agent 演练场 · 阶段1（agent 网关：把记录/回放/基线/task 判定包成机器可解析的 JSON）
#
# 目的：让 agent（LLM/脚本）不必解析人读的字符串（`[selftest] PASS (6 checks)`、
#       `== A] …ALARM…`），而是通过统一网关拿到结构化 JSON。
# 原则：判据单一来源，全部复用 arena/gate.py + run.py + task.py；本文件只做"命令 -> JSON"胶水。
#
# 统一 JSON 信封（stdout 只放这一份 JSON，任何诊断走 stderr）：
#   { "ok": true, "cmd": "<subcommand>", "data": {…}, "stdout": "<该命令原来打印的内容>" }
#   失败时：{ "ok": false, "cmd": "<subcommand>", "error": "<原因>", "exit": <0/1/2> }
#
# 子命令：
#   status  <db>                  DB 概览：session/run 总数、最近若干 run
#   rebuild <db> [--kind 模式]    契约指纹/输出量基线判定（等价 baseline_check --kind）-> {base, alarms:[]}
#   submit  <task.json> --run <dir> [--base <dir>]  对一轮 transcript 跑 task 判分 -> {verdict, gates:[]}
#   task    [list|<task.json>]    列出可用任务/查看单个 task 契约
#   gates                         列出已注册判据
#
# 用法（产物默认在 build/，可用 BUILD 环境变量改目录）：
#   python3 tests/arena/qw.py status "${BUILD:-build}/torture/torture.sqlite"
#   python3 tests/arena/qw.py submit tests/arena/tasks/arena-001-torture-a.json \
#       --run "${BUILD:-build}/transcripts/torture-a-XXX" --base "${BUILD:-build}/transcripts/torture-a-YYY"
#   exit: 0=ok / 1=判定 FAIL 或命令失败 / 2=参数/用法错误
import argparse
import json
import sqlite3
import sys
from pathlib import Path

# 使本目录可被当包 import（复用 gate/run/task，单一判据来源）
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate import GATES, COUNT_TOL  # noqa: E402
from run import build_run  # noqa: E402
from task import load_task, judge  # noqa: E402
from evaluate import evaluate  # noqa: E402

ALARM_PREFIX = "ALARM"


# ---------------- 诊断辅助 ----------------
def _diag(msg):
    """诊断信息走 stderr，保持 stdout 只有 JSON。"""
    print(msg, file=sys.stderr)


# ---------------- 命令实现（返回可直接 JSON 化的 data） ----------------

def cmd_status(db):
    con = sqlite3.connect(db)
    con.row_factory = sqlite3.Row
    runs = list(con.execute(
        "SELECT runid,created_at,result,in_count,out_lines,out_bytes,contract_hash "
        "FROM transcripts ORDER BY created_at DESC LIMIT 20"))
    stage_count = con.execute(
        "SELECT count(*) FROM stage_timing").fetchone()[0]
    kinds = list(con.execute(
        "SELECT substr(runid,1,length(runid)-15) AS k, count(*) n "
        "FROM transcripts GROUP BY k ORDER BY n DESC"))
    con.close()
    return {
        "db": str(db),
        "runs_total": "见 kinds",
        "stage_timing_rows": stage_count,
        "run_kinds": [{"kind": r["k"], "n": r["n"]} for r in kinds],
        "recent_runs": [
            {
                "runid": r["runid"], "created_at": r["created_at"], "result": r["result"],
                "in_count": r["in_count"], "out_lines": r["out_lines"],
                "out_bytes": r["out_bytes"], "contract_hash": (r["contract_hash"] or "")[:8],
            }
            for r in runs
        ],
    }


def cmd_rebuild(db, kind):
    """等价 baseline_check 的 A 段 + stages：契约指纹/输出量基线判定。"""
    con = sqlite3.connect(db)
    con.row_factory = sqlite3.Row
    runs = list(con.execute(
        "SELECT runid,created_at,contract_hash,out_lines,out_bytes "
        "FROM transcripts WHERE runid LIKE ? ORDER BY created_at, runid", (kind,)))
    if not runs:
        con.close()
        return {"kind": kind, "note": "无匹配 run，先 tr2sqlite 导入"}, False
    base = runs[0]
    base_run = {"contract_hash": base["contract_hash"],
                "out_lines": base["out_lines"], "out_bytes": base["out_bytes"]}
    rows = []
    alarm_count = 0
    for r in runs[1:]:
        cur_run = {"contract_hash": r["contract_hash"],
                   "out_lines": r["out_lines"], "out_bytes": r["out_bytes"]}
        gates = {
            "contract_drift": (gate_contract_drift(cur_run, base_run)),
            "output_lines_drift": (gate_output_lines_drift(cur_run, base_run)),
            "output_bytes_drift": (gate_output_bytes_drift(cur_run, base_run)),
        }
        ok_ = all(ok for ok, _ in gates.values())
        alarm_count += 0 if ok_ else 1
        rows.append({
            "runid": r["runid"], "ok": ok_,
            "out_lines": r["out_lines"], "out_bytes": r["out_bytes"],
            "contract_hash": (r["contract_hash"] or "")[:8],
            "gates": [{"name": k, "ok": ok, "msg": msg} for k, (ok, msg) in gates.items()],
        })
    # 阶段耗时趋势（若存在 stage_timing）
    stages = []
    try:
        ss = sorted({s for (s,) in con.execute(
            "SELECT DISTINCT stage FROM stage_timing WHERE runid LIKE ?", (kind,))})
        for st in ss:
            rows_ = list(con.execute(
                "SELECT runid,wall_ms FROM stage_timing WHERE runid LIKE ? AND stage=? "
                "ORDER BY runid", (kind, st)))
            vals = [w for _, w in rows_]
            stages.append({
                "stage": st, "p50": percentile(vals, 50), "p95": percentile(vals, 95),
                "latest": {"runid": rows_[-1][0], "wall_ms": rows_[-1][1]},
            })
    except Exception as e:  # stage_timing 可能尚未建/导入
        _diag(f"[qw] stages 读取跳过: {e}")
    con.close()
    return {
        "kind": kind,
        "base": {"runid": base["runid"], "contract_hash": (base["contract_hash"] or "")[:8],
                 "out_lines": base["out_lines"], "out_bytes": base["out_bytes"]},
        "alarm_count": alarm_count,
        "runs": rows,
        "stages": stages,
    }, alarm_count == 0


def cmd_submit(task_path, run_dir, base_dir=None):
    """对一轮 transcript 跑 task 判定。复用 task.judge（单一判据）。"""
    try:
        task = load_task(task_path)
    except ValueError as e:
        return {"task": str(task_path), "error": str(e)}, False
    run = build_run(run_dir)
    base = None
    if base_dir:
        base = build_run(base_dir)
    elif task.get("base") and task["base"].get("runid"):
        p = Path(run_dir).parent / task["base"]["runid"]
        if p.exists():
            base = build_run(p)
    results, passed = judge(task, run, base)
    return {
        "task": task["id"], "title": task.get("title", ""),
        "run": run["runid"], "base_runid": (base or {}).get("runid"),
        "verdict": "PASS" if passed else "FAIL",
        "gates": results,
    }, passed


def cmd_task(task_arg):
    """task list -> 列出任务；task <json> -> 读出契约。"""
    tasks_dir = Path(__file__).resolve().parent / "tasks"
    if task_arg == "list" or task_arg is None:
        files = sorted(tasks_dir.glob("*.json")) if tasks_dir.is_dir() else []
        if not files:
            return {"tasks": [], "note": "tests/arena/tasks/ 下暂无任务"}, True
        out = []
        for f in files:
            try:
                t = json.loads(f.read_text(encoding="utf-8"))
            except Exception:
                continue
            out.append({"id": t.get("id", f.stem), "title": t.get("title", ""),
                        "layer": t.get("layer"), "difficulty": t.get("difficulty"),
                        "gates": t.get("gates"),
                        "file": str(f)})
        return {"tasks": out}, True
    # 否则按 task.json 路径读
    tp = Path(task_arg)
    if not tp.exists():
        return {"error": f"task 不存在: {task_arg}"}, False
    try:
        t = load_task(tp)
    except ValueError as e:
        return {"task": task_arg, "error": str(e)}, False
    return {"task": t}, True


def cmd_gates():
    return {"gates": sorted(GATES)}, True


def cmd_eval(run, task_json, base, replay_log):
    """阶段2评测器：对一轮 transcript 跑全判据 -> {verdict, steps, tr}。"""
    task = load_task(task_json) if task_json else None
    base_run = build_run(base) if base else None
    ev = evaluate(run, task=task, base=base_run, replay_log=replay_log)
    return ev, (ev["verdict"] == "PASS")


# ---------------- 判据复用（与 baseline_check 同源） ----------------
# 直接 import 自 gate，避免本文件重复实现；此处仅为局部别名可读
from gate import (  # noqa: E402
    gate_contract_drift,
    gate_output_lines_drift,
    gate_output_bytes_drift,
)


def percentile(vals, p):
    if not vals:
        return 0
    s = sorted(vals)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    frac = k - lo
    return s[lo] + (s[hi] - s[lo]) * frac


# ---------------- 主入口 ----------------
COMMANDS = {"status", "rebuild", "submit", "task", "eval", "gates"}


def main() -> int:
    p = argparse.ArgumentParser(description="agent 演练场网关：把 record/replay 分析封成 JSON")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp_status = sub.add_parser("status", help="DB 概览")
    sp_status.add_argument("db")

    sp_rebuild = sub.add_parser("rebuild", help="契约指纹/输出量基线判定")
    sp_rebuild.add_argument("db")
    sp_rebuild.add_argument("--kind", default="torture-a%")

    sp_submit = sub.add_parser("submit", help="对一轮 transcript 跑 task 判分")
    sp_submit.add_argument("task")
    sp_submit.add_argument("--run", required=True, help="当轮 transcript 目录")
    sp_submit.add_argument("--base", help="基线 transcript 目录（可选）")

    sp_task = sub.add_parser("task", help="列出任务 / 查看单个任务")
    sp_task.add_argument("task", nargs="?", default="list")

    sp_eval = sub.add_parser("eval", help="阶段2评测器：判定闭环 PASS/WARN/FAIL")
    sp_eval.add_argument("run", help="当轮 transcript 目录")
    sp_eval.add_argument("--task", help="task.json（baseline 判据集，缺省全集）")
    sp_eval.add_argument("--base", help="基线 transcript 目录")
    sp_eval.add_argument("--replay-log", help="replay 输出日志（给则做 replay 差分）")

    sub.add_parser("gates", help="列出已注册判据")

    args = p.parse_args()

    try:
        if args.cmd == "status":
            data, ok = cmd_status(args.db), True
        elif args.cmd == "rebuild":
            data, ok = cmd_rebuild(args.db, args.kind)
        elif args.cmd == "submit":
            data, ok = cmd_submit(args.task, args.run, args.base)
        elif args.cmd == "task":
            data, ok = cmd_task(args.task)
        elif args.cmd == "eval":
            data, ok = cmd_eval(args.run, args.task, args.base, args.replay_log)
        elif args.cmd == "gates":
            data, ok = cmd_gates()
        else:
            p.print_help()
            return 2
    except sqlite3.Error as e:
        _emit({"ok": False, "cmd": args.cmd, "error": f"sqlite: {e}", "exit": 1})
        return 1
    except Exception as e:
        _emit({"ok": False, "cmd": args.cmd, "error": str(e), "exit": 1})
        return 1

    _emit({"ok": ok, "cmd": args.cmd, "data": data})
    return 0 if ok else 1


def _emit(obj):
    print(json.dumps(obj, ensure_ascii=False))


if __name__ == "__main__":
    sys.exit(main())