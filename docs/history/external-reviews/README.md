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
| F-4 | v0.33 | **BUG-042** | selftest 汇总行被内核异步打印撕裂 | `16e8f82`（PR #8 已合 main） | |
| F-5 | v0.33 | **BUG-043** | pid 表耗尽静默返回 | `16e8f82`（PR #8 已合 main） | |
| F-6 | — | **OBS-004** | writefile 128B 行截断 | 限速不修 | F-3 修复后已非崩溃引信，仅教学限制 |
| 评审残留 | — | **OBS-003** | netsock send/recv 无进程归属 | 威胁模型声明 | close 已隔离（BUG-038），send/recv 保持共享语义 |
| F1–F6 | v1.4.8(#28) | （工具链/测试基建，待立项为修补 PR） | RR 地基：回放/录制无输入背压(P1)、HTTP_PORT 不贯穿、golden 无窗口、网络流无格式 | 见 `mini-os-rr-handoff-for-dev_bd2f598a.md` | 仅登记核实与处置，代码修复另拆 PR 后再补 commit |
| OBS-MM-1 | v1.5 | （观察/加固，非 bug） | 内核集中式堆只增不还帧池；kmalloc 依赖连续帧竞争帧池 | 见 `mini-os-mm-subsystem-deep-audit_d3b0c68.md` | P3，教学定位可推迟 |
| OBS-MM-2 | v1.5 | （观察/加固，非 bug） | frame_alloc O(nframes) 首次适配未声明复杂度假设 | 同上 | P3 |
| OBS-MM-3 | v1.5 | （hygiene，非 bug） | mem_init 位图清零到 nframes/8 字节边界 | 同上 | P3 |
| OBS-MM-4 | v1.5 | （观察，非 bug） | ZOMBIE 延迟持有的页目录/页表帧直到 reap | 同上 | P2 |
| OBS-MM-5 | v1.5 | （重构建议，非 bug） | 每进程资源配额散落定义，应收敛为 resource_t | 同上 | P2，呼应架构总览 |
| OBS-NET-1 | v1.5 | （潜在契约缺口，非已证缺陷） | TCP 上行滑动窗口槽复用未断言 busy==0（部分 ACK 下可能覆盖在途槽） | `mini-os-netstack-deep-audit.md` | P2，验证性加固 |
| OBS-NET-2 | v1.5 | （观察，非 bug） | 可靠下行 rx_next/seq 16 位回绕未文档化 | 同上 | P3 |
| OBS-NET-3 | v1.5 | （假设声明，非 bug） | 突发下行依赖 NIC 环 + 快速 recv | 同上 | P3 |
| OBS-NET-4 | v1.5 | （加固，非 bug） | auto_port 冲突审计 + 回绕处理 | 同上 | P3 |
| OBS-CC-1 | v1.5 | （崩溃引信，静态推演） | cc500 递归下降无深度护栏，深嵌套可耗尽栈 | `mini-os-cc500-deep-audit.md` | P2，加护栏 |
| OBS-CC-2 | v1.5 | （hygiene，非 bug） | realloc newlen≥oldlen 隐式契约无断言 | 同上 | P3 |
| OBS-CC-3 | v1.5 | （文档，非 bug） | 单栈全局方案、非短路位运算边界 | 同上 | P3 |
| OBS-CC-4 | v1.5 | （资源，非 bug） | 部分失败路径 fs slot 未归还 | 同上 | P3 |
| OBS-CC-5 | v1.5 | （文档，非 bug） | 32KB 输入上限声明 | 同上 | P3 |
| OBS-RR-1 | v1.5 | （加固，非 bug） | RR ack 信号依赖 debug 关键词，建议独立通道 | `mini-os-rr-engine-deep-audit.md` | P3 |
| OBS-RR-2 | v1.5 | （能力，非 bug） | 回放 ack 未齐仅告警，建议 RP_ACK_STRICT | 同上 | P3 |
| OBS-RR-3 | v1.5 | （文档，非 bug） | in/out.tr header 加格式版本 | 同上 | P3 |
| OBS-DRV-1 | v1.5 | （验证性加固，静态推演） | e1000_tx 忙等 30 万次可能阻塞 timer ISR 上下文 | `mini-os-drv-deep-audit.md` | P2，TX 移出 ISR 或轻量化 |
| OBS-DRV-2 | v1.5 | （防御，非 bug） | ata 层 lba+count 无越界校验 | 同上 | P3 |
| OBS-DRV-3 | v1.5 | （hygiene，非 bug） | serial_printf 内嵌 serial_puts 二次取关中断 | 同上 | P3 |
| OBS-DRV-4 | v1.5 | （契约文档，非 bug） | e1000 TX 同步轮询契约未注明 | 同上 | P3 |
| OBS-FS-1 | v1.5 | （能力，静态推演） | 目录无间接块扩容，条目上限约 1536 | `mini-os-fs-deep-audit.md` | P2 |
| OBS-FS-2 | v1.5 | （性能，非 bug） | 持久化全量同步无脏块位图 | 同上 | P2 |
| OBS-FS-3 | v1.5 | （防御，非 bug） | bitmap set/test 无越界护栏 | 同上 | P3 |
| OBS-FS-4 | v1.5 | （一致性，非 bug） | fs_read/fs_write 双界不对称 | 同上 | P3 |
| OBS-FS-5 | v1.5 | （文档，非 bug） | storage_sync 非原子语义未声明 | 同上 | P3 |
| OBS-ARCH-1 | v1.5 | （文档，非 bug） | PIC 从片全掩码，IRQ8-15 永不触发需显式注明 | `mini-os-arch-deep-audit.md` | P3 |
| OBS-ARCH-2 | v1.5 | （契约，非 bug） | sched_switch_esp 裸 uint32_t 入参无类型安全 | 同上 | P3 |
| OBS-ARCH-3 | v1.5 | （加固，非 bug） | boot.s hang 前后无诊断输出 | 同上 | P3 |
| OBS-ARCH-4 | v1.5 | （防御，非 bug） | timer_init freq=0 除零保护 | 同上 | P3 |
| OBS-KP-1 | v1.5 | （文档，静态推演） | 等待队列数组前移 O(n) 出队 + 上限语义 | `mini-os-kernelprim-deep-audit.md` | P2，注明复杂度与上限 |
| OBS-KP-2 | v1.5 | （防御，非 bug） | msg 交棒不变量未断言 | 同上 | P3 |
| OBS-KP-3 | v1.5 | （可读性，非 bug） | sem 三态返回改枚举显式化 | 同上 | P3 |
| OBS-KP-4 | v1.5 | （自审计，非 bug） | sched_audit 未对账就绪队列与 PCB 状态 | 同上 | P3 |
| OBS-SH-1 | v1.5 | （观察，非 bug） | shell 命令 15 项线性匹配 | `mini-os-shell-deep-audit.md` | P3 |
| OBS-SH-2 | v1.5 | （防御，非 bug） | writefile 单行内容长度未显式校验 | 同上 | P3 |
| OBS-SH-3 | v1.5 | （可读性，非 bug） | DELIM/PATH 上限魔法数字分散 | 同上 | P3 |
| OBS-SH-4 | v1.5 | （一致性，非 bug） | tokenize/split_* 双解析风格并存 | 同上 | P3 |

## 非独立 bug 的演进记录

| 主题 | 归档位置 | commit（本地→合并） |
|---|---|---|
| per-process fd 表（fd 号进程私有，根治 BUG-031 的全局槽污染） | changelog v0.31 **Changed** | `6160db8` → merge `b37e331` |

## 归档说明

外部报告原始 findings（`socket-findings.md` / `cc500-findings.md` / 本批任务包）由评审方提供时，
原样归档于本目录（各自独立 `.md`，保留来源标注与时间戳）。此 README 为对照索引，不替代原文。

> **命名规则（治理 P2 明文化，2026-09-03）**：审计对象为**单点 commit** 的报告，文件名带对象
> sha 后缀（如 `mini-os-arch-and-quality-review_6ac70e4.md`，对象 `6ac70e4`）；审计对象为**代码
> 目录 / 多文件**（无单点 sha 可指）的深审报告省略后缀，其对象在报告头"审计对象"行标注路径与行数。
> 本目录 2026-09-03 由 `docs/external-reviews/` 迁入 `docs/history/external-reviews/`（时点产物
> 归只读归档区，不占活文档区），报告正文原样未动。

> 顶层**架构与代码质量综合评审**（评分 8.3/10）：`mini-os-arch-and-quality-review_6ac70e4.md`
> （对象 `6ac70e4`，含总体定位/架构分层/验证体系/约束声明）。各子系统专项深审见上方 OBS-* 行，本报告为总览。