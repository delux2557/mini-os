# diffsynth 审计包

对 `delux2557/mini-os` → `v2-c-kernel/tools/minicc/diffsynth/`（minicc 的随机差分对拍网，基线 commit `387ef62`）的独立审计。

## 内容

| 文件 | 说明 |
|---|---|
| `diffsynth-审计报告.md` | 主报告：四组件架构审计、1,150 例生成器实测数据（gcc 接受率/确定性/UBSan/运行语义差分）、覆盖面审计（9 个 minicc 缺陷 0/9 可被生成——逐条对照表）、CAPS_CC500 过期、UB 纪律论证缺口、ddmin 依赖问题、按性价比排序的建议 |
| `harness/hostminicc32` / `runmin32` | **预编译二进制**（静态、无 libc 依赖，28K/12K）：被测编译器 + 产物执行器，解压即可用（需 x86-64 Linux 内核开启 ia32 执行，默认开启） |
| `harness/crt32.c` / `runmin.c` / `start32.s` | 上述二进制的源码（预编译二进制不可用时，设 `MINICC_SRC` 指向 mini-os 仓库即可从源重建） |
| `harness/gen_audit.sh` | 审计复现脚本：把 diffsynth 生成器的产出接进"运行语义差分环"（gcc 参考 vs minicc 产物真实执行），兼做确定性双跑与 UBSan 检查 |
| `harness/dd_pred.sh` | `ddmin.sh` 的自定义谓词示例：运行语义差分判定（绕开其默认的 acceptance-only 谓词与 QEMU 依赖） |
| `evidence/run_seed*.log` | 4 组参数的实测日志（合计 1,150 例：gcc_rej=0 / nondet=0 / UB=0 / minicc_rej=0 / 语义差分=0 / 崩溃=0） |

## 一句话结论

**"回归网"合格，"发现网"不足**：生成器在自身形态空间内确定、无 UB、零假差分（工程细节多有惊喜）；但已知 9 个 minicc 缺陷的触发形态 0/9 可被生成——空条件 `for`、长标识符、超大数组维度、arity 错配、NUL、八进制、深嵌套全部在模板形态之外。另有 `CAPS_CC500` 停留在 2026-09-05 校准，cc500 M1–M6 增量在差分网中零覆盖。

## 复现

```bash
# 依赖：gcc（native）。二进制已随包附带 → 解压即可跑；若不可用，设 MINICC_SRC 指向 mini-os 仓库从源重建。
# 1) 生成器审计（四组参数）
N=400 SEED=100 VARS=6  STMTS=8  bash gen_audit.sh
N=250 SEED=7   VARS=8  STMTS=30 bash gen_audit.sh
# 2) ddmin 自定义谓词验证（二进制在 harness/ 内，无需另配）
HMINICC=$PWD/harness/hostminicc RUNMIN=$PWD/harness/runmin32 \
  DD_PROG=$PWD/harness/dd_pred.sh bash /path/to/diffsynth/ddmin.sh <可疑样本.c>
```

## 评估边界

未运行 `run_diff.sh` / `test_diffsynth_guest.sh` 全流程（无 QEMU）；其判据结论来自脚本审计。"不可生成"结论基于 gen.c 模板静态读取 + 语料 grep 复核。
