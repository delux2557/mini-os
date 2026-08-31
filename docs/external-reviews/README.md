# 外部评估报告索引（External Reviews 账本）

> 目的：把各轮外部评估报告里的**缺陷 ID（F-x / OBS-y）**与仓库内**正式 BUG 编号 / 观察记录 /
> 版本 / commit** 一一对账，使外部报告 ↔ bugs.md ↔ changelog ↔ git 历史可互相检索。
> 约定：编号以**已合入 main 的 bugs.md** 为准（部分外部报告用不同临时编号，如 cc500 曾标
> 038a/b/c，已合为 039/040/041——见下表"说明"列）。

## 缺陷对照表

| 外部报告 ID | 版本 | bugs.md 编号 | 内容 | 修复 commit（本地→合并） | 说明 |
|---|---|---|---|---|---|
| F-0a | v0.31 | **BUG-037** | socket 退出回收（防槽位泄漏） | `94c9425` → merge `63aca39` | |
| F-0b | v0.31 | **BUG-038** | socket 归属校验 + DHCP 保留槽防任意 close | 同上 | |
| F-0c | v0.31 | （工程，非 bug） | net sock 审计收口 + 表满专项日志 | 同上 | 归 changelog v0.31 Engineering |
| F-1 | v0.32 | **BUG-041** | cc500 关系运算补齐 + error() 带 token 上下文 | `fc441cf` → merge `85cc213` | 外部报告曾标 038a |
| F-2 | v0.32 | **BUG-040** | cc500 只声明未定义函数静默编出 call-ELF 头废产物 | 同上 | 外部报告曾标 038b |
| F-3 | v0.32 | **BUG-039** | cc500 未闭合字符串字面量越界自噬 | 同上 | 外部报告曾标 038c |
| F-4 | v0.33 | **BUG-042** | selftest 汇总行被内核异步打印撕裂 | `b78edd3`（PR #8，待合） | |
| F-5 | v0.33 | **BUG-043** | pid 表耗尽静默返回 | `b78edd3`（PR #8，待合） | |
| F-6 | — | **OBS-004** | writefile 128B 行截断 | 限速不修 | F-3 修复后已非崩溃引信，仅教学限制 |
| 评审残留 | — | **OBS-003** | netsock send/recv 无进程归属 | 威胁模型声明 | close 已隔离（BUG-038），send/recv 保持共享语义 |

## 非独立 bug 的演进记录

| 主题 | 归档位置 | commit（本地→合并） |
|---|---|---|
| per-process fd 表（fd 号进程私有，根治 BUG-031 的全局槽污染） | changelog v0.31 **Changed** | `6160db8` → merge `b37e331` |

## 归档说明

外部报告原始 findings（`socket-findings.md` / `cc500-findings.md` / 本批任务包）由评审方提供时，
原样归档于本目录（各自独立 `.md`，保留来源标注与时间戳）。此 README 为对照索引，不替代原文。