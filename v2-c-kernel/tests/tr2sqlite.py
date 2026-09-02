#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/tr2sqlite.py
# record/replay 地基 · 分析索引（旁路"放大镜"）
#
# 设计要点（对现有 P2/P3 零侵入，纯增量工具）：
#   * 录放主路径仍是 .tr 文本（证据原件：确定性/可 diff/可归档），本脚本不读不写它。
#   * sqlite 只是 out-of-band 的只读索引，坏了/删了绝不影响录放正确性。
#   * 幂等：按 runid DELETE + INSERT，可重复跑、可增量补 runid。
#
# 字段（schema）：
#   transcripts  —— 归档元数据/血统（每个 transcript 目录一行）
#   in_events    —— 输入事件（.in.tr）：seq / rel_ms / cmd(首词)/ payload
#   out_rows     —— 输出逐行（.out.tr）：line_no / content（可 LIKE 检索）
#
# 用法：
#   python3 tests/tr2sqlite.py DB path/to/runid [runid...]   # 导入指定 transcript
#   python3 tests/tr2sqlite.py --dirs DB transcript_base     # 扫描目录下所有 runid
#   python3 tests/tr2sqlite.py -q DB "SELECT ..."            # 便捷查询
#   python3 tests/tr2sqlite.py --demo DB                     # 打印示例查询结果
import argparse
import sqlite3
import sys
import time
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS transcripts(
  runid     TEXT PRIMARY KEY,
  created_at TEXT,
  result    TEXT,
  in_count  INTEGER,
  out_bytes INTEGER,
  out_lines INTEGER,
  src_in    TEXT,
  src_out   TEXT
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
CREATE INDEX IF NOT EXISTS ix_in_events_cmd ON in_events(runid, cmd);
CREATE INDEX IF NOT EXISTS ix_out_rows_line ON out_rows(runid, line_no);
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
    for t in ("transcripts", "in_events", "out_rows"):
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
    if out_tr.exists():
        raw = out_tr.read_bytes()
        lines = raw.decode("utf-8", errors="replace").split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        conn.executemany(
            "INSERT INTO out_rows VALUES(?,?,?)",
            [(runid, i, c) for i, c in enumerate(lines)],
        )
        out_bytes, out_lines = len(raw), len(lines)
    else:
        out_bytes, out_lines = 0, 0

    conn.execute(
        "INSERT INTO transcripts(runid,created_at,result,in_count,out_bytes,out_lines,src_in,src_out) "
        "VALUES(?,?,?,?,?,?,?,?)",
        (runid, created, result, len(ev), out_bytes, out_lines, str(in_tr), str(out_tr)),
    )
    conn.commit()
    return dict(runid=runid, in_count=len(ev), out_lines=out_lines, out_bytes=out_bytes)


def demo(conn: sqlite3.Connection) -> None:
    print("== transcripts ==")
    for r in conn.execute("SELECT runid,created_at,result,in_count,out_lines FROM transcripts ORDER BY runid"):
        print("  %-24s %s %-4s in=%-4d out_lines=%d" % r)
    print("== 命令直方图（in_events 聚合） ==")
    for r in conn.execute("SELECT cmd,count(*) c,min(rel_ms) lo,max(rel_ms) hi "
                          "FROM in_events GROUP BY cmd ORDER BY c DESC"):
        print("  %-14s count=%-3d rel_ms[%d..%d]" % r)
    print("== 全程时间跨度 ==（末条 rel_ms 即相对首条的毫秒）")
    for r in conn.execute("SELECT runid,min(rel_ms),max(rel_ms) FROM in_events GROUP BY runid"):
        print("  %-24s span=%dms" % (r[0], r[2] - r[1]))
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
            print(f"[ok] {s['runid']:<24} in={s['in_count']:<4} out_lines={s['out_lines']:<5} out_bytes={s['out_bytes']}")
        except Exception as e:  # noqa: BLE001
            print(f"[fail] {t}: {e}", file=sys.stderr)

    if args.demo:
        demo(conn)
    conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())