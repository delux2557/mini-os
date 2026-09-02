#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/tr2sqlite.py
# record/replay 地基 · 分析索引（旁路"放大镜"）· v2 (L1 完整版)
#
# 设计要点（对现有 P2/P3 零侵入，纯增量工具）：
#   * 录放主路径仍是 .tr 文本（证据原件：确定性/可 diff/可归档），本脚本不读不写它。
#   * sqlite 只是 out-of-band 的只读索引，坏了/删了绝不影响录放正确性。
#   * 幂等：按 runid DELETE + INSERT，可重复跑、可增量补 runid。
#
# v2 新增（L1：帮"定位哪个环节慢" + 契约指纹基线）：
#   * stage_timing  —— 阶段耗时表（compile/boot/exec/finalize），导入 transcript 目录里的 stages.tsv。
#                      `stage \t wall_ms`，逐 run 记账 → 跨轮 P50/P95 趋势，看"哪段变慢"。
#   * transcripts.contract_hash —— 功能契约指纹：把函数契约行(exited code/ISOLATED OK/PASS/
#                      verify/…，先按 norm() 去掉后台 demo tick 噪声行) 归一化排序后整段 sha256。
#                      新跑一轮哈希不等 = 基线告警（静默功能回归的强信号）。
#
# 字段（schema）：
#   transcripts  —— 归档元数据/血统 + 产出基线(contract_hash/out_bytes/out_lines)
#   in_events    —— 输入事件（.in.tr）：seq / rel_ms / cmd(首词)/ payload
#   out_rows     —— 输出逐行（.out.tr）：line_no / content（可 LIKE 检索）
#   stage_timing —— 阶段耗时：runid / stage / wall_ms
#
# 用法：
#   python3 tests/tr2sqlite.py DB path/to/runid [runid...]   # 导入指定 transcript
#   python3 tests/tr2sqlite.py --dirs DB transcript_base     # 扫描目录下所有 runid
#   python3 tests/tr2sqlite.py -q DB "SELECT ..."            # 便捷查询
#   python3 tests/tr2sqlite.py --demo DB                     # 打印示例查询结果
import argparse
import hashlib
import re
import sqlite3
import sys
import time
from pathlib import Path

# ---- 契约指纹的判定逻辑：必须与 tests/rp_torture.sh 的 func() / norm() 对齐，避免漂移 ----
# norm()：去掉已知后台 demo 的 tick 噪声行
_DROP_TICK = re.compile(r'^\[[AB]\] tick=')
# func() 的功能契约行（exited code / ISOLATED OK / …）
CONTRACT_RE = re.compile(
    r"'[a-z0-9/_.-]+' exited code=[0-9]+$"
    r"|ISOLATED OK|byte-identical|verify OK|can't load|FAILED to exec|wrote [0-9]+ bytes"
)


def contract_lines(lines):
    """对输出行做 norm 归一化后，挑出功能契约行（对应 rp_torture.sh 的 func()）。"""
    sel = []
    for l in lines:
        if _DROP_TICK.match(l):
            continue  # demo tick 噪声
        if CONTRACT_RE.search(l):
            sel.append(l)
    return sel


def contract_hash(lines):
    """契约指纹：契约行(去重、排序)整段 SHA-256。新 run 哈希变化 = 功能契约/输出漂移的强信号。"""
    canonical = sorted(set(contract_lines(lines)))
    return hashlib.sha256("\n".join(canonical).encode()).hexdigest()


SCHEMA = """
CREATE TABLE IF NOT EXISTS transcripts(
  runid     TEXT PRIMARY KEY,
  created_at TEXT,
  result    TEXT,
  in_count  INTEGER,
  out_bytes INTEGER,
  out_lines INTEGER,
  src_in    TEXT,
  src_out   TEXT,
  contract_hash TEXT
);
CREATE TABLE IF NOT EXISTS in_events(
  runid   TEXT NOT NULL,
  seq     INTEGER,
  rel_ms  INTEGER,
  cmd     TEXT,
  payload TEXT,
  PRIMARY KEY(runid, seq)
);
CREATE TABLE IF NOT EXISTS out_rows(
  runid   TEXT NOT NULL,
  line_no INTEGER,
  content TEXT,
  PRIMARY KEY(runid, line_no)
);
CREATE TABLE IF NOT EXISTS stage_timing(
  runid   TEXT NOT NULL,
  stage   TEXT NOT NULL,
  wall_ms INTEGER,
  PRIMARY KEY(runid, stage)
);
CREATE INDEX IF NOT EXISTS ix_in_events_cmd ON in_events(runid, cmd);
CREATE INDEX IF NOT EXISTS ix_out_rows_line ON out_rows(runid, line_no);
CREATE INDEX IF NOT EXISTS ix_stage_stage ON stage_timing(stage, wall_ms);
"""


def read_header(in_tr: Path) -> dict:
    h = {}
    for line in in_tr.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("#"):
            for k in ("runid", "result"):
                if k in line and "=" not in line:
                    tail = line[len(f"# {k}: "):].strip()
                    h[k] = tail
    return h


def import_runid(conn: sqlite3.Connection, d: Path) -> dict:
    """导入单个 transcript 目录；返回统计。幂等：先删该 runid 旧行再插。"""
    in_tr = d / "in.tr"
    out_tr = d / "out.tr"
    if not in_tr.exists():
        raise FileNotFoundError(f"缺 in.tr: {in_tr}")

    runid = d.name
    hdr = read_header(in_tr)
    if hdr.get("runid"):
        runid = hdr["runid"]

    result = hdr.get("result", "")
    res_file = d / "RESULT"
    if res_file.exists():
        for line in res_file.read_text(errors="replace").splitlines():
            if line.startswith("# result:"):
                result = line.split(":", 1)[1].strip()
                break

    created = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(in_tr.stat().st_mtime))

    # 清旧行（幂等，可重跑 / 可增量补）
    for t in ("transcripts", "in_events", "out_rows", "stage_timing"):
        conn.execute(f"DELETE FROM {t} WHERE runid=?", (runid,))

    # 输入事件：`seq \t rel_ms \t payload`；# 开头的 header/注释跳过
    ev = []
    for line in in_tr.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t", 2)
        if len(parts) < 3:
            continue
        try:
            seq, rel = int(parts[0]), int(parts[1])
        except ValueError:
            continue
        payload = parts[2] if len(parts) == 3 else ""
        cmd = payload.split(None, 1)[0] if payload.strip() else ""
        ev.append((runid, seq, rel, cmd, payload))
    conn.executemany("INSERT INTO in_events VALUES(?,?,?,?,?)", ev)

    # 输出逐行：.out.tr 原始字节按行拆入，支持 LIKE 检索
    out_lines_txt = []
    if out_tr.exists():
        raw = out_tr.read_bytes()
        lines = raw.decode("utf-8", errors="replace").split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        out_lines_txt = lines
        conn.executemany(
            "INSERT INTO out_rows VALUES(?,?,?)",
            [(runid, i, c) for i, c in enumerate(lines)],
        )
        out_bytes, out_lines = len(raw), len(lines)
    else:
        out_bytes, out_lines = 0, 0

    # 契约指纹（L1）：新 run 哈希变化 = 功能契约/输出漂移强信号
    chash = contract_hash(out_lines_txt)

    # 阶段耗时（L1）：导入 stages.tsv（`stage \t wall_ms`，来自 rp_torture.sh 打点）
    stages = []
    stages_tsv = d / "stages.tsv"
    if stages_tsv.exists():
        for line in stages_tsv.read_text(errors="replace").splitlines():
            if not line or line.startswith("#"):
                continue
            p = line.split("\t")
            if len(p) >= 2:
                try:
                    stages.append((runid, p[0].strip(), int(p[1].strip())))
                except ValueError:
                    pass
        if stages:
            conn.executemany("INSERT INTO stage_timing VALUES(?,?,?)", stages)

    conn.execute(
        "INSERT INTO transcripts(runid,created_at,result,in_count,out_bytes,out_lines,src_in,src_out,contract_hash) "
        "VALUES(?,?,?,?,?,?,?,?,?)",
        (runid, created, result, len(ev), out_bytes, out_lines, str(in_tr), str(out_tr), chash),
    )
    conn.commit()
    return dict(runid=runid, in_count=len(ev), out_lines=out_lines,
                out_bytes=out_bytes, contract_hash=chash[:8])


def demo(conn: sqlite3.Connection) -> None:
    print("== transcripts ==")
    for r in conn.execute("SELECT runid,created_at,result,in_count,out_lines,substr(contract_hash,1,8) "
                          "FROM transcripts ORDER BY runid"):
        print("  %-24s %s %-4s in=%-4d out_lines=%-5d hash=%s" % r)
    print("== 命令直方图（in_events 聚合） ==")
    for r in conn.execute("SELECT cmd,count(*) c,min(rel_ms) lo,max(rel_ms) hi "
                          "FROM in_events GROUP BY cmd ORDER BY c DESC"):
        print("  %-14s count=%-3d rel_ms[%d..%d]" % r)
    print("== 阶段耗时（stage_timing） ==")
    for r in conn.execute("SELECT runid,stage,wall_ms FROM stage_timing ORDER BY runid,stage"):
        print("  %-24s %-10s %dms" % r)
    print("== 输出里 mini-os 提示符出现次数（发行版诊断） ==")
    for r in conn.execute("SELECT runid,count(*) FROM out_rows WHERE content LIKE '%mini-os$ %' GROUP BY runid"):
        print("  %-24s prompts=%d" % r)


def main() -> int:
    p = argparse.ArgumentParser(description="把 .tr transcript 增量导入 sqlite 分析索引")
    p.add_argument("db", help="sqlite 文件路径")
    p.add_argument("targets", nargs="*", help="transcript 目录 或 --dirs 参数指定的根")
    p.add_argument("--dirs", metavar="BASE", help="扫描该目录下所有 transcript（每个子目录 = 一个 runid）")
    p.add_argument("-q", "--query", help="便捷查询：SELECT ...")
    p.add_argument("--demo", action="store_true", help="打印示例查询结果")
    p.add_argument("--schema", action="store_true", help="只建表/清 schema 后退出")
    args = p.parse_args()

    conn = sqlite3.connect(args.db)
    conn.executescript(SCHEMA)
    conn.commit()
    if args.schema:
        print(f"schema 就绪: {args.db}")
        conn.close()
        return 0

    if args.schema is False and args.query:
        for row in conn.execute(args.query):
            print(row if len(row) > 1 else row[0])
        conn.close()
        return 0

    dirs = list(args.targets)
    if args.dirs:
        base = Path(args.dirs)
        dirs += [str(x) for x in sorted(base.iterdir()) if x.is_dir() and (x / "in.tr").exists()]

    if not dirs:
        print("未指定任何 transcript 目录（给路径或用 --dirs）。", file=sys.stderr)
        return 2

    for t in dirs:
        d = Path(t)
        try:
            s = import_runid(conn, d)
            print(f"[ok] {s['runid']:<24} in={s['in_count']:<4} out_lines={s['out_lines']:<5} "
                  f"out_bytes={s['out_bytes']:<7} hash={s['contract_hash']}")
        except Exception as e:  # noqa: BLE001
            print(f"[fail] {t}: {e}", file=sys.stderr)

    if args.demo:
        demo(conn)
    conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())