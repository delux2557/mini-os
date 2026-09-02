#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/arena/evaluate.py
# agent 演练场 · 阶段2（评测器 / 判定闭环）
#
# 把"agent 改完内核、跑一轮 transcript"后该得的判决串成一条闭链：
#   评测 <run_dir>  ==  逐真值判据叠加，自动得 PASS / WARN / FAIL，
#   并落到"在哪一步 + .tr 证据"上——agent 只需读 verdict + steps 就知道该看哪里。
#
# 判据维度（复用判据单一来源 gate/run/task，本文件只加"证据采集 + 汇总"）：
#   * result    读转录目录 RESULT 的 `# result:`——自身生命周期判定（挂了/崩了 = 前提 FAIL）
#   * baseline  contract_hash / out_lines / out_bytes / 契约内容 对照基线 run（按 task.gates 过滤）
#   * audit     扫 out.tr 的内核致命/越权/溢出标记 + 内核自审计失败行；已知预期隔离演示(procCrash)排除
#   * replay    （可选：给 --replay-log 才判）重放契约行集合 vs 当轮契约行集合——现场能否复原
#
# 汇总：任一 FAIL -> FAIL；无 FAIL 但有 WARN -> WARN；否则 PASS。
# 本文件可直接独立跑（给普通人读的表格 / exit code），也可经 qw.py eval 包成 agent JSON。
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate import (  # noqa: E402
    GATES,
    COUNT_TOL,
    contract_lines,
    run_gates,
)
from run import build_run  # noqa: E402
from task import load_task  # noqa: E402

# 派生自 rp_torture.sh kmark() 的标记集合（保持同源：FATAL/越权/溢出/panic/BUG）
MARKER_RE = re.compile(
    r"\[FATAL\]|double free|PAGE FAULT pid=|STACK OVERFLOW pid=|panic|PANIC|BUG")
# rp_torture.sh B段已知预期：procCrash 故意越权做隔离演示，其 FAULT+kill 不算缺陷
EXPECTED_CTX = re.compile(r"crash demo: writing kernel memory")
# 内核自审计失败信号（存在 "[audit] mem ok" 这类 PASS 行；只挑显式失败行）
AUDIT_FAIL_RE = re.compile(
    r"\[audit\]\s*[^:]*:\s*(FAIL|ERROR)|\[audit\]\s+\w+\s+FAIL|\[selftest\]\s+FAIL")

DEFAULT_GATES = ["contract_drift", "output_lines_drift",
                 "output_bytes_drift", "contract_content"]


# ---------------- 各判定维度（返回 {status, evidence}) ----------------

def step_result(run_dir):
    """RESULT 文件 `# result:` 生命周期判定。"""
    r = Path(run_dir) / "RESULT"
    if not r.exists():
        return "WARN", "缺 RESULT 文件，无法判自身生命周期"
    txt = r.read_text(errors="replace")
    for line in txt.splitlines():
        if line.startswith("# result:"):
            val = line.split(":", 1)[1].strip().upper()
            if val == "PASS":
                return "PASS", f"RESULT: # result: {val}"
            return "FAIL", f"RESULT: # result: {val}"
    return "WARN", "RESULT 无 # result: 行"


def step_baseline(run, base, task):
    """契约/输出量基线判定，复用 gate.run_gates（单一判据来源）。"""
    if base is None:
        return "WARN", "无基线 run，跳过 baseline 判定"
    wanted = set(task.get("gates", DEFAULT_GATES)) if task else set(DEFAULT_GATES)
    results = [g for g in run_gates(run, base, task.get("tolerance", COUNT_TOL) if task else COUNT_TOL)
               if g["name"] in wanted]
    fails = [g for g in results if not g["ok"]]
    if fails:
        msgs = " | ".join(f"{g['name']}: {g['msg']}" for g in fails)
        return "FAIL", f"baseline({len(fails)}/{len(results)} gate FAIL): {msgs}"
    return "PASS", "baseline 通过: " + "; ".join(f"{g['name']}={g['msg']}" for g in results)


def _is_expected(marker_line, prevtwo):
    """FATAL/PAGE FAULT 标记是否属于已知预期隔离演示(procCrash)：前 2 行含 crash demo 越权上下文。"""
    return bool(EXPECTED_CTX.search(marker_line)) or any(EXPECTED_CTX.search(p) for p in prevtwo)


def step_audit(out_lines):
    """扫输出里的内核致命/越权/溢出标记 + 自审计失败行；给出行号证据。"""
    suspects = []   # (lineno, normalized_line)
    audit_fails = []
    for i, line in enumerate(out_lines, start=1):
        if MARKER_RE.search(line):
            if not _is_expected(line, out_lines[max(0, i - 3):i - 1]):
                suspects.append((i, line.strip()))
        if AUDIT_FAIL_RE.search(line):
            audit_fails.append((i, line.strip()))
    ev = []
    if suspects:
        ev.append("内核致命/越权/溢出标记: " + "; ".join(f"L{n} {l}" for n, l in suspects))
    if audit_fails:
        ev.append("内核自审计失败: " + "; ".join(f"L{n} {l}" for n, l in audit_fails))
    if not ev:
        return "PASS", "无内核致命标记、自审计通过"
    return "FAIL", " | ".join(ev)


def step_replay(run_lines, replay_log):
    """重放证据：重放契约行集合 vs 当轮契约行集合，能否复原现场。"""
    if not replay_log:
        return "WARN", "未提供 --replay-log，replay 差分跳过"
    rl = Path(replay_log)
    if not rl.exists():
        return "FAIL", f"replay-log 不存在: {replay_log}"
    replay_lines = rl.read_text(errors="replace").split("\n")
    run_contract = sorted(set(contract_lines(run_lines)))
    rep_contract = sorted(set(contract_lines(replay_lines)))
    if run_contract == rep_contract:
        return "PASS", f"重放契约行集合与当轮一一对应（{len(run_contract)} 条 ISOLATED/exited-code 复现）"
    miss_run = sorted(set(run_contract) - set(rep_contract))
    miss_rep = sorted(set(rep_contract) - set(run_contract))
    parts = []
    if miss_run:
        parts.append("当轮有replay缺: " + "; ".join(miss_run[:3]))
    if miss_rep:
        parts.append("replay有当轮缺: " + "; ".join(miss_rep[:3]))
    return "FAIL", "复原分歧: " + "; ".join(parts)


# ---------------- 评测器主入口 ----------------

def evaluate(run_dir, task=None, base=None, replay_log=None):
    """对一轮 transcript 跑全判据。返回 {verdict, steps:[{step,status,evidence}], tr}。"""
    d = Path(run_dir)
    run = build_run(run_dir)
    out_tr = d / "out.tr"
    out_lines = []
    if out_tr.exists():
        out_lines = out_tr.read_bytes().decode("utf-8", errors="replace").split("\n")
        if out_lines and out_lines[-1] == "":
            out_lines.pop()

    steps = []
    for name, fn, args in (
        ("result", step_result, (run_dir,)),
        ("baseline", step_baseline, (run, base, task)),
        ("audit", step_audit, (out_lines,)),
        ("replay", step_replay, (out_lines, replay_log)),
    ):
        st, evid = fn(*args)
        steps.append({"step": name, "status": st, "evidence": evid})
    statuses = {s["status"] for s in steps}
    if "FAIL" in statuses:
        verdict = "FAIL"
    elif "WARN" in statuses:
        verdict = "WARN"
    else:
        verdict = "PASS"
    return {
        "runid": run["runid"], "base_runid": (base or {}).get("runid"),
        "task": (task or {}).get("id"),
        "verdict": verdict,
        "steps": steps,
        "tr": {"out.tr": str(out_tr), "in.tr": str(d / "in.tr"),
               "RESULT": str(d / "RESULT")},
    }


def _print_human(ev):
    print(f"runid   : {ev['runid']}")
    print(f"base    : {ev['base_runid']}")
    print(f"task    : {ev['task']}")
    print(f"verdict : {ev['verdict']}")
    for s in ev["steps"]:
        print(f"  [{s['status']:<4}] {s['step']:<8} {s['evidence']}")
    print("tr evidence: " + ", ".join(f"{k}={v}" for k, v in ev["tr"].items()))


def main() -> int:
    p = argparse.ArgumentParser(description="agent 演练场 · 阶段2评测器（判定闭环）")
    p.add_argument("run", help="当轮 transcript 目录")
    p.add_argument("--task", help="task.json（baseline 判据集；缺省用全集）")
    p.add_argument("--base", help="基线 transcript 目录")
    p.add_argument("--replay-log", help="replay 输出日志（给则做 replay 差分）")
    args = p.parse_args()

    task = load_task(args.task) if args.task else None
    base = build_run(args.base) if args.base else None
    ev = evaluate(args.run, task=task, base=base,
                  replay_log=args.replay_log if args.replay_log else None)
    ev["runner"] = "evaluate.py"
    _print_human(ev)
    return 0 if ev["verdict"] == "PASS" else (2 if ev["verdict"] == "WARN" else 1)


if __name__ == "__main__":
    sys.exit(main())