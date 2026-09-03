# mini-os v2-c-kernel RR 回放引擎专项深审报告

> **来源**：代码审查专家（本轮专项审计）
> **审计对象**：`v2-c-kernel/tests/transcript.sh`（录制侧，156 行）+ `tests/replay.sh`（回放侧，91 行）+
> 消费方（`rp_torture.sh`/`test_replay.sh`/`test-tr` 相关回归）
> **前置**：上一轮外部交接单 `mini-os-rr-handoff-for-dev_bd2f598a.md` 提出 F1~F6，其中 F1/F2/F4
> 已由 v1.4.8/v1.5 修复；本审计**核实修复落地 + 登记残余**。
> **方法**：逐行读码 + ack 背压时序推演 + 与交接单 F1~F6 逐项对照。

---

## 0. 结论

RR 地基的**核心正确性缺口已闭环**：F1（回放无输入背压，P1）和 F2（录制无 ack，P2）均已用
「readline 消费 ack（`[sched] wake keyboard waiter` / `[kb] readline`）计数」作为两条独立节拍钳制；
F4（golden 无窗口概念）已由 `tr_mark` + `tr_window_after` 锚点机制解决。**F1 修复经时序推演正确**
（回放逐行等"前一条已消费"再注入当前条），无 P1/P2 残余。登记 `OBS-RR-x` 观察项 3 项（均 P3，
顺带确认 F3/F5/F6 追踪状态）。

---

## 1. 对象与边界

- 环境：记录=墙钟相对毫秒（`TR_NOW` 游标）；回放=ack 背压节拍 + 可选 `REPLAY_ICOUNT=1` icount
  确定性时钟。
- 方法：静态读码 + 时序推演；**未跑 QEMU 动态回放**。

## 2. 交接单 F1~F6 落地核实

| 项 | 定义 | 现况 | 核实 |
|---|---|---|---|
| F1 [P1] 回放无背压吞行 | 只按 rel_ms sleep | **已修复**：`replay_into` 逐行等 `base+acked`（前条已消费）再注当前条（[replay.sh:73-79](file:///workspace/mini-os/v2-c-kernel/tests/replay.sh#L71-L80)）。时序推演：注入#2 前等 #1 ack → 等"前一条消费"语义成立，无跨行合并 | 正确 |
| F2 [P2] 录制无 ack | golden 残缺 | **已修复**：`tr_send` 注入后 `tr_ack_wait(need+1)` 才记 rel_ms + 返回（[transcript.sh:96-103](file:///workspace/mini-os/v2-c-kernel/tests/transcript.sh#L93-L103)）；超时 exit 2 | 正确 |
| F4 [P3] golden 无窗口 | 跨轮判据脆 | **已修复**：`tr_mark` 命名锚固化 log_off/rel_ms（[transcript.sh:116-125](file:///workspace/mini-os/v2-c-kernel/tests/transcript.sh#L116-L125)）+ `tr_window_after` 从锚后 tail -c（[transcript.sh:128-137](file:///workspace/mini-os/v2-c-kernel/tests/transcript.sh#L128-L137)）；in.tr 写 `# MARK` 注释行为回放侧跳过 | 正确 |
| F3 [P2] HTTP 端口不贯穿 | 假阳性 | **未修（工具链域）**：HTTP_PORT 仍需贯穿各 demo/转发器——属 F3 追踪项，未在本轮 | 追踪中 |
| F5 能力缺口 | 网络流无格式 | 未做（能力缺口非缺陷）；transcript 只覆盖串口通道 | 明确范围 |
| F6 注记 | 使用 | 保持 | 确认 |

## 3. 发现项（`OBS-RR-*`）

### OBS-RR-1【P3】TR_ACK_RE 依赖日志关键词，遇 BUG-051 类撕裂仍可能误判
ack 计数基于 `grep -cE` 串口日志里的 `[sched] wake keyboard waiter...` / `[kb] readline...` 两串。
BUG-051 已做整行 IRQ 原子化（serial 撕裂封堵），ack 行本身可靠；但**关键词由驱动日志格式决定**，
若某内核版本改打印或不打这两条（如优化/裁剪），RR 的背压即失效而没有任何显式告警。建议：
把 ack 信号做成**独立、稳定、不随 debug 日志演化的通道**（如专用 filter_kmsg 标记），令背压不依赖
易变关键词。

### OBS-RR-2【P3】`replay_into` 对 ack 未齐仅告警不失败（`continue` 语义）
超时未齐 ack 时（[replay.sh:76-77](file:///workspace/mini-os/v2-c-kernel/tests/replay.sh#L75-L77)）打印告警后
**仍注入当前行**——设计上允许慢 icount 继续，代价是可能吞行但被标记。对"确定性差分"用例，吞行即
差分失真，建议：Replay 侧提供 `RP_ACK_STRICT=1` 时把未齐 ack 升级为失败，供确定性断言严格化。

### OBS-RR-3【P3】in.tr/out.tr 行规约无 schema 版本
header 注明了 `seq \t rel_ms \t payload` 与"payload 禁 TAB/换行"规约，但无版本字段。若未来改成
多行编码/加列，旧 transcript 会被静默误读（F4 已给 `# MARK` 前导，但整体格式版本未固化）。建议
在 header 加 `# fmt:<version>`，回放侧据此容错。

## 4. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| OBS-RR-1 | P3 | 加固 | ack 信号独立稳定通道，不依赖 debug 关键词 |
| OBS-RR-2 | P3 | 能力 | RP_ACK_STRICT 严格化回放（确定性断言） |
| OBS-RR-3 | P3 | 文档 | in/out.tr header 加格式版本 |

*注：本轮 RR 专项基于静态读码 + 时序推演；F1/F2/F4 修复经逻辑核验为正确。F3（HTTP 端口贯穿）为
已知未修追踪项，建议单独立项（工具链 demo 域）。*