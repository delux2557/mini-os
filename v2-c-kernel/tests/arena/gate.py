#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/arena/gate.py
# agent 演练场 · 阶段0（task 契约 + 可复用 gate 判据）
#
# 判据(gate) = 一条可独立复用、可被任务门禁引用的"判定函数"。
# 本文件把 baseline_check.py 里"契约指纹 / 输出量漂移"的判定逻辑抽成
# 纯函数，供：
#   1) baseline_check.py  跨轮基线巡检（回归已有的 exit-code 门禁）
#   2) agent 演练场 task.json 的 gate 引用（阶段0契约）
# 统一复用，避免判据在多个入口漂移。
#
# 约定：每个 gate 都是 `call(run, base, tol) -> (ok: bool, msg: str)`。
#   * run    = 当轮 run 的判定数据 dict（见 Run 数据契约）
#   * base   = 基线 run 数据 dict（可为 None，表示无基线）
#   * tol    = 漂移容忍度（多数 gate 默认使用 COUNT_TOL）
#   * 返回   = (是否通过, 给人/agent 读的判定说明)
import hashlib
import re

# 判定数据契约（Run 数据 dict 的字段，由各入口构建）：
#   {
#     "contract_hash": "<sha256>",   # 功能契约指纹
#     "out_lines":     <int>,         # 输出总行数
#     "out_bytes":     <int>,         # 输出总字节数
#   }
# baseline_check.py 从 sqlite 构建；arena 从 transcript 目录构建。

COUNT_TOL = 0.20  # 输出行数/字节数相对基线允许的漂移（与 baseline_check 原值一致）

# 与 tr2sqlite 对齐的契约判定：去掉后台 demo tick 噪声行，再挑功能契约行
_DROP_TICK = re.compile(r"^\[[AB]\] tick=")
CONTRACT_RE = re.compile(
    r"'[a-z0-9/_.-]+' exited code=[0-9]+$"
    r"|ISOLATED OK|byte-identical|verify OK|can't load|FAILED to exec|wrote [0-9]+ bytes"
)


def contract_lines(lines):
    """归一化后挑出功能契约行（对应 rp_torture.sh 的 func()）。"""
    sel = []
    for l in lines:
        if _DROP_TICK.match(l):
            continue
        if CONTRACT_RE.search(l):
            sel.append(l)
    return sel


def contract_hash(lines):
    """契约指纹：契约行(去重、排序)整段 SHA-256。"""
    canonical = sorted(set(contract_lines(lines)))
    return hashlib.sha256("\n".join(canonical).encode()).hexdigest()


def _rel_delta(a, b):
    """a/b 相对偏差（b 为 0 时按极大告警）。"""
    return abs(a - b) / float(b or 1)


# ---- 判据（gate）实现 ----

def gate_contract_drift(run, base, tol=0.0):
    """功能契约指纹漂移。run.contract_hash != base.contract_hash 即告警。"""
    if not base or not base.get("contract_hash"):
        return True, "无基线，跳过契约判定"
    if run.get("contract_hash") == base.get("contract_hash"):
        return True, "契约指纹一致"
    return False, "ALARM 契约指纹漂移"


def gate_output_lines_drift(run, base, tol=COUNT_TOL):
    """输出行数相对基线漂移。"""
    if not base or not base.get("out_lines"):
        return True, "无基线输出行数，跳过"
    if not run.get("out_lines"):
        return True, "本输出行数未知"
    if _rel_delta(run["out_lines"], base["out_lines"]) > tol:
        return False, f"ALARM 输出行数跳变 {base['out_lines']}->{run['out_lines']}"
    return True, f"输出行数一致({run['out_lines']} vs {base['out_lines']})"


def gate_output_bytes_drift(run, base, tol=COUNT_TOL):
    """输出字节数相对基线漂移。"""
    if not base or not base.get("out_bytes"):
        return True, "无基线输出字节数，跳过"
    if not run.get("out_bytes"):
        return True, "本输出字节数未知"
    if _rel_delta(run["out_bytes"], base["out_bytes"]) > tol:
        return False, f"ALARM 输出字节跳变 {base['out_bytes']}->{run['out_bytes']}"
    return True, f"输出字节一致({run['out_bytes']} vs {base['out_bytes']})"


def gate_contract_content(run, base, tol=None):
    """契约内容出现/丢失（run 少了基线里存在的某条契约行 = 漂移）。"""
    if not base or not base.get("contract_lines"):
        return True, "无基线契约内容，跳过"
    base_lines = set(base.get("contract_lines", []))
    run_lines = set(run.get("contract_lines", []))
    missing = sorted(base_lines - run_lines)
    if missing:
        return False, "ALARM 契约行丢失: " + "; ".join(missing[:3])
    return True, "基线契约行全部保留"


# gate 注册表：任务门禁按名字引用
GATES = {
    "contract_drift": gate_contract_drift,
    "output_lines_drift": gate_output_lines_drift,
    "output_bytes_drift": gate_output_bytes_drift,
    "contract_content": gate_contract_content,
}


def run_gate(name, run, base, tol=COUNT_TOL):
    """按名字执行单个 gate，返回 (ok, msg)。name 未知则抛 KeyError（由调用方兜底）。"""
    return GATES[name](run, base, tol)


def run_gates(run, base, tol=COUNT_TOL):
    """逐个执行所有 gate，返回 [{name, ok, msg}, ...]。"""
    return [{"name": n, "ok": ok, "msg": msg}
            for n, ok, msg in
            [(n, *g(run, base, tol)) for n, g in GATES.items()]]


# 兼容：baseline_check.py 里用到的"是否产生告警"聚合判断
def is_alarm(results):
    return any(not r["ok"] for r in results)