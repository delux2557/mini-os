# mini-os v2-c-kernel 内核原语（sched/sem/msg）专项深审报告

> **来源**：代码审查专家（本轮专项审计）
> **审计对象**：`v2-c-kernel/src/kernel/`（sched_policy.c 62 行 / sem.c 46 行 / msg.c 93 行 /
> sched.h PCB 状态机）+ 宿主单测（test_sched/test_sem/test_msg 已有覆盖）
> **方法**：逐行读码 + 状态机/唤醒语义/队列结构推演 + 与 BUG-004/005/014/030 对照。
> **边界**：纯静态读码，未动态运行。sched.c 的 fork/exec/terminate/reap 已在 mm 专项深审覆盖，
> 本审计聚焦"调度策略 + 同步原语"纯逻辑层。

---

## 0. 结论

**同步/调度原语为"纯逻辑、可宿主单测"设计典范**：`sched_policy`（就绪环形队列）、`sem`（计数+等待队列）、
`msg`（有界环形缓冲+双 FIFO 等待队列）均不依赖内核，配 `sched_audit/sem_invariant_ok` 自审计。
唤醒语"交棒"、阻塞原因枚举（防误唤）、PID 状态机严格合法。**无 P1/P2 已证缺陷**，登记 `OBS-KP-x`
观察项（1×P2 + 3×P3）。

---

## 1. 对象与边界

- 结构：`pcb_t` 含 5 态状态机（FREE/READY/RUNNING/BLOCKED/ZOMBIE）+ 8 种阻塞原因 + 每进程 fd 表。
- 方法：静态读码 + 同步语义推演；未动态运行。

## 2. 架构正向核实

- **就绪队列环形数组 + remove 前移**：`policy_readyq_remove` 找到目标后把后续元素逐格前移、`tail` 回退
  （[sched_policy.c:43-59](file:///workspace/mini-os/v2-c-kernel/src/kernel/sched_policy.c#L43-L59)），维持连续
  布局无空洞；`contains` O(n) 只读。`POLICY_MAX_READY` 满则 `push -1`，无越界。
- **sem 语义严格**：`sem_wait_try` count>0 降 / 否则入队返回"应阻塞"；`sem_signal_wake` 队首唤醒计数
  不变 / 无人等待则 count++。`sem_invariant_ok` 校验 count≥0、队不溢出、且 **count>0 时不得有等待者**
  （防 signal 丢失，[sem.c:41-45](file:///workspace/mini-os/v2-c-kernel/src/kernel/sem.c#L41-L45)）。
- **msg 交棒唤醒正确**：`msg_send_wake` 在消费者非空时把队首消息直接交付消费者（缓冲不滞留）；
  `msg_recv_wake` 把暂存生产者消息搬入缓冲并唤醒。经典管程交棒，无消息丢失/重复。
- **阻塞原因枚举防误唤**：sem/msg/sleep/keyboard/wait 用 `block_reason` 区分唤醒路径（BUG-004 定时
  误唤已隔），`sched_wake_with` 可带返回值（eax）精确恢复阻塞 syscall 语义（BUG-005）。
- **宿主单测覆盖**：test_sched/test_sem/test_msg 直接编译这些纯逻辑文件，秒级验证。

## 3. 发现项（`OBS-KP-*`）

### OBS-KP-1【P2】所有等待队列均为"数组 + 队首前移"的 O(n) 出队
`sem` 的 `waiters[]`、`msg` 的 `producers/consumers[]`、`sched_policy` 的 `ready[]` remove 都靠**前移
后续元素**维持 FIFO（O(n)），且 `MAX_WAITERS`/`POLICY_MAX_READY` 有硬上限。当前演示规模（MAX_PROCS=16、
等待者少）无碍，但**任一队列满载 + 高频操作会退化为线性**。对齐教学定位可接受；建议：在头文件注明
"等待队列为数组前移、O(n) 出队"，并说明 `MAX_WAITERS` 上限即并发同步参与者的近似上界，避免未来
误当 O(1) 队列扩展。

### OBS-KP-2【P3】`msg_send_wake`/`msg_recv_wake` 依赖"缓冲恰为空/非空"的不变量无需断言
`msg_send_wake` 注释了"缓冲此刻必非空（消费者只在空缓冲等待）"（[msg.c:59-63](file:///workspace/mini-os/v2-c-kernel/src/kernel/msg.c#L57-L67)）；
`msg_recv_wake` 校验 `count < capacity`。该不变量由调用序保证但**未编码为断言**。P3：加 `assert(count>0)`/
`assert(count<capacity)` 让逆变触发即显形。

### OBS-KP-3【P3】`sem_wait_try` 队满返回 -1 与"应阻塞(1)"、"占用成功(0)"三值并存
调用方（syscall 层）需区分 0/1/-1 三种语义。当前接口返回 -1 表示"等待队列已满，视为失败"，语义靠调用方
记忆。P3：建议改枚举常量（如 `SEM_OK/SEM_SHOULD_BLOCK/SEM_NO_SLOT`）显式化，减少误判。

### OBS-KP-4【P3】`sched_audit` 只校验 PCB 状态机的独立合法性，未校验就绪队列一致性
`sched_audit`（[sched.c:774-786](file:///workspace/mini-os/v2-c-kernel/src/kernel/sched.c#L774-L786)）检查状态
枚举/idle 恒 RUNNING/单 RUNNING/block_reason 合法，但**未对账「就绪队列内 pid 集合」与「各进程
state==READY」是否一致**。若某烂路径 push 了 RUNNING 进程或漏 pop，audit 不报。P3：可加一遍
`policy_readyq_contains` 对账，使就绪队列与 PCB 状态一致性也可验证。

## 4. 与历史 bug 对照

| 历史 | 现况 | 状态 |
|---|---|---|
| BUG-004 信号量等待者被定时误唤 | block_reason 隔离唤醒路径 | 已封堵 |
| BUG-005 阻塞 syscall 唤醒返回值错误 | sched_wake_with 带 eax | 已封堵 |
| BUG-014 sys_wait 的 spawn/wait 竞态 | v0.14 孤儿化+wait 语义 | 已封堵 |
| BUG-030 fork 继承栈误判 | guard 槽位改 stack_bottom | 已封堵 |

## 5. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| OBS-KP-1 | P2 | 文档 | 等待队列 O(n) 出队 + 上限语义注明 |
| OBS-KP-2 | P3 | 防御 | msg 交棒不变量断言 |
| OBS-KP-3 | P3 | 可读性 | sem 三态返回改枚举 |
| OBS-KP-4 | P3 | 自审计 | sched_audit 加就绪队列与状态对账 |

*注：全项静态推演；当前演示规模（≤16 进程、同步参与少）均正确，观察项为演进/可读性加固。*