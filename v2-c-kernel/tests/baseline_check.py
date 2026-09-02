#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/baseline_check.py
# record/replay 地基 · L1 分析层 · 跨轮基线巡检
#
# 一份 run 的产出要能回答两个问题：("哪个环节变慢了？")("功能契约/输出量有没有漂移？")
# 本脚本基于 tr2sqlite.py 灌入的 sqlite 分析索引做只读巡检，不写回任何录放主路径。
#
# A) 契约指纹与输出量基线
#    以"最早记录的 runid"为基线(golden)，逐个后续 run 比对：
#      * contract_hash  不等 -> 强信号 ALARM(功能契约/输出静默漂移)
#      * out_lines/out_bytes  相对基线变化 > COUNT_TOL(20%) -> ALARM(输出量异常, 如之前抓到的
#                                                                      "同一命令集 out_lines 832->1165 跳变")
#      * 判 after_hash  哈希变化但计数不变 -> 契约行重排/新增契约(警示)
# B) 阶段耗时跨轮趋势
#    对 stage_timing 逐 stage 算跨轮 P50/P95，标出最新 run 是否贴近/超 P95(变慢)。
#    wall_ms 是 host 墙钟、受 icount sleep=on 对齐，只当趋势信号，不作精确契约。
#
# 用法：
#   python3 tests/baseline_check.py DB [--kind 'torture-a%'] [--alarm] [--stages]
#     --kind   runid LIKE 过滤，把同组 run 放一起比（默认 '%' 全量）
#     --alarm  任一 ALARM 项存在则 exit 1（可接门禁；默认仅打印 exit 0）
#     --stages 输出阶段耗时趋势（默认只做 A)
#   exit: 0=无告警 / 1=有告警(仅 --alarm 时)
import argparse
import sqlite3
import sys

# 判定判据统一复用 arena/gate.py，避免多入口漂移
sys.path.insert(0, __file__.rsplit("/", 1)[0] + "/arena")
from gate import COUNT_TOL, gate_contract_drift, gate_output_lines_drift, gate_output_bytes_drift  # noqa: E402

ALARM_PREFIX = "ALARM"


def percentile(vals, p):
    """无 scipy 依赖的百分位：p∈[0,100]。空列表返回 0。"""
    if not vals:
        return 0
    s = sorted(vals)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    frac = k - lo
    return s[lo] + (s[hi] - s[lo]) * frac


def check(conn, kind, show_stages, gate_alarm):
    runs = list(conn.execute(
        "SELECT runid,created_at,contract_hash,out_lines,out_bytes "
        "FROM transcripts WHERE runid LIKE ? ORDER BY created_at, runid", (kind,)))
    if not runs:
        print(f"[info] 无 runid LIKE '{kind}' 的数据——先跑 tr2sqlite 导入。")
        return 0

    base = runs[0]
    base_run = {"contract_hash": base[2], "out_lines": base[3], "out_bytes": base[4]}
    print("== A] 契约指纹 / 输出量基线（基线 = 最早 runid）==")
    print(f"{'runid':<26}{'out_lines':>10}{'out_bytes':>10}  {'hash':<9} {'' :<22} 判定")
    print(f"{base[0]:<26}{base[3]:>10}{base[4]:>10}  {base[2][:8]:<9}  (基线)")
    alarms = []
    for r in runs[1:]:
        runid, created, h, nlines, nbytes = r
        cur_run = {"contract_hash": h, "out_lines": nlines, "out_bytes": nbytes}
        tags = []
        for ok, msg in (
            gate_contract_drift(cur_run, base_run),
            gate_output_lines_drift(cur_run, base_run),
            gate_output_bytes_drift(cur_run, base_run),
        ):
            if not ok:
                tags.append(msg)
        mark = " | ".join(tags) if tags else "一致"
        print(f"{runid:<26}{nlines:>10}{nbytes:>10}  {h[:8]:<9}  {mark}")
        alarms.extend(tags)
    # 汇总给 stage 提示一条具体的告警样例
    if show_stages:
        print()
        print("== B] 阶段耗时跨轮趋势（stage_timing; P50/P95; 最新 run 贴边即更慢预警）==")
        stages = sorted(
            {s for (s,) in conn.execute(
                "SELECT DISTINCT stage FROM stage_timing WHERE runid LIKE ?", (kind,))})
        if not stages:
            print("    暂无 stage_timing——需后续 run 由 rp_torture.sh 打点 stages.tsv。")
        for st in stages:
            rows = list(conn.execute(
                "SELECT runid,wall_ms FROM stage_timing WHERE runid LIKE ? AND stage=? "
                "ORDER BY runid", (kind, st)))
            vals = [w for _, w in rows]
            p50, p95 = percentile(vals, 50), percentile(vals, 95)
            latest = rows[-1]
            flag = ""
            if p95 and latest[1] > p95 * 1.05:
                flag = "  <- 超 P95，变慢预警"
            print(f"  {st:<12} P50={p50:>6}ms  P95={p95:>6}ms  最新[{latest[0][-15:]}]={latest[1]}ms{flag}")
    return 1 if (alarms and gate_alarm) else 0


def main() -> int:
    p = argparse.ArgumentParser(description="跨轮基线巡检：契约指纹/输出量 + 阶段耗时趋势")
    p.add_argument("db", help="sqlite 文件路径（tr2sqlite 灌入）")
    p.add_argument("--kind", default="%", help="runid LIKE 过滤（默认 %% 全量）")
    p.add_argument("--alarm", action="store_true", help="存在告警时 exit 1")
    p.add_argument("--stages", action="store_true", help="同时输出阶段耗时跨轮趋势")
    args = p.parse_args()
    conn = sqlite3.connect(args.db)
    conn.row_factory = sqlite3.Row
    rc = check(conn, args.kind, args.stages, args.alarm)
    conn.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())