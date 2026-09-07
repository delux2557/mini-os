# mini-os v2-c-kernel 网络栈（net/TCP）专项深审报告

> **来源**：代码审查专家（本轮专项审计）
> **审计对象**：`v2-c-kernel` 工作树（`feat/bug053-build-isolation` PR 合入后，当前 `main` 基底）
> **审计范围**：`src/net/`（netif/ip/udp/icmp/dhcp/slip/netsock）+ `src/app/tcp.c`（虚拟 TCP 薄封装）+
> `src/net/tcp_proto.h`（会话协议）+ 链路适配层（`drv/e1000_netif.c`/`drv/uart_netif.c`）
> **方法**：逐行读码 + 跨子系统调用链推演 + 与既有 bug 记录（BUG-018~020/029/044~050）对照。

---

## 0. 结论

网络栈**分层抽象质量高**（netif ops 表 + 协议层零网卡依赖），且在真实缺陷驱动下完成了多轮
正确的可靠性迭代（BUG-047 丢尾 → TCP_RXB 扩 + 单报泵取；BUG-050 排空挤爆 socket 环 → 单帧泵取；
v1.2 可靠下行 stop-and-wait + 上行累计 ACK）。**无 P1/P2 级可确定缺陷**，登记 4 项 `OBS-NET-x`
观察/加固项。

---

## 1. 对象与边界

- 环境：内核侧 guest 虚拟 TCP + 宿主转发器（`tests/tcp_proxy.py`）+ e1000/SLIP 双链路。
- 方法：静态读码 + 状态机/窗口/环推演；**未运行**，结论为架构与正确性评估，非动态缺陷证明。

## 2. 架构正向核实

- **D6 网卡抽象成立**：`netif_register/init_all/tx/rx/mac` 转发入口 + 注册表，
  `netsock`/`tcp.c` 只依赖接口；`e1000_netif`/`uart_netif` 为唯一网卡实现。协议层零具体符号
  （回归测试已断言 `src/net` 无 `e1000_*` 直调）。
- **MTU 硬墙分层钳制**：`TCP_MTU(1400)` 单报墙在 `tcp_send` 本地早返；netsock sendto/recvfrom
  再钳制，双墙不重叠依赖。
- **可靠性环路闭环**：下行 `MSG_DATA(seq) → rx_push → send_ack(期望 next)`（stop-and-wait）；
  上行 `MSG_ACK(next) → tx_ack 推进 tx_base`（滑动窗口 TCP_TXWIN=8），超时 `tx_retrans` 幂等重传。
- **事件/数据分离队**：状态事件队 `ev`（满则覆盖最旧 latest-wins，绝不丢），数据 `rxb` 环（可牺牲）。
  TCP/RECV 恒三态（>0 数据 / 0 closed / -1 err|timeout）互斥。

## 3. 发现项（`OBS-NET-*`）

### OBS-NET-1【P2】上行滑动窗口槽复用未显式断言互斥
`tcp_send` 写 `tx_win[tx_seq % TCP_TXWIN]`（tcp.c:196）**不检查该槽 `busy==0`**。
窗口满（`tx_inflight >= 8`）时让步阻塞可保满窗不覆盖；但**部分 ACK（一次只确认 1）后 inflight 降到 <8 时**，
写入槽 `tx_seq % 8 = (tx_base+inflight)%8` 可能落在「仍 busy 的在途槽」上，静默覆盖未确认载荷副本，
而 `tx_seq` 已推进——重传时该 seq 发的是新载荷；转发器遇乱序 seq 会丢弃并回累计 ACK，**端到端靠
幂等自愈兜住**，但等价于"重传语义被破坏、窗口吞吐退化"。建议：
- `tcp_send` 写槽前加 `if (s->busy) return -1;`（或断言），把"环复用必须落在已 ACK 槽"变成可强制契约；
- 或在 `tx_ack` 里保证：只有 `adv` 使 `tx_seq%8` 对应槽空闲时才允许继续写。

### OBS-NET-2【P3】reliable downsend `rx_next/rx_seq` 16 位回绕未处理
`rx_next++`（tcp.c:131）与转发器发 seq 同为 16 位，理论 >70000 包（≈8.5MB @1400B）后二者同步回绕，
依赖两端同段无界递增。当前无碍，但未文档化为"回绕由转发器与 guest 同步维护"之不变量。建议在
`tcp-session-proto.md` 显式声明 seq 为"无限增长的 mod-2^16"，并注释该假设。

### OBS-NET-3【P3】`netsock_drain`/`drain` 单报泵取把突发缓冲完全交给链路反应时间
单帧泵取（BUG-050 修复）在 socket/tcp 环安全与 NIC 环容量间做了折中：若宿主突发下大量帧且
guest 未及时 recv，e1000 256 槽 / SLIP 缓冲耗尽后后续帧被 DMA 丢弃。对当前演示流量成立；
建议记录"突发下行依赖 NIC 环深度承载 + app 快速 recv"为已知假设，勿在批量下行场景误当无损。

### OBS-NET-4【P3】`auto_port` 单调递增不复用且回绕未护
`netsock_open` 的 `auto_port`（21000 起）用完 100 次尝试后若仍冲突则失败；不回落复用端口。
数量上限（NET_SOCK_MAX 槽）+ 递增分配下冲突概率低，但 `auto_port++` 若 16 位计满回绕至已占用段
未显式处理。建议在端口重复时记录审计信息而非仅当次失败。

## 4. 与历史 bug 对照（防回归锚点）

| 历史 | 现况 | 状态 |
|---|---|---|
| BUG-018/019 e1000 读写易失/使能位 | volatile + TCTL/RCTL 修正 | 已封堵 |
| BUG-020 QEMU RCTL 收包排队 | 适配层兼容 | 已封堵 |
| BUG-029 icmp 短帧越界 | 宿主 fuzz 抓到 + 修 | 已封堵 |
| BUG-044 转发器"主循环阻塞 recv 饿死串口" | send/recv 解耦 | 已封堵 |
| BUG-045 SLIP 通道失败根因 | 队列/唤醒修正 | 已封堵 |
| BUG-047 TCP 大响应丢尾 | TCP_RXB 4K→16K + drain 单报 | 已封堵 |
| BUG-049 httpdemo/dldemo 进度伪迹 | 协议对齐 | 已封堵 |
| BUG-050 e1000 CLOSED 被挤爆 | netsock 单帧泵取 | 已封堵 |

## 5. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| OBS-NET-1 | P2 | 正确性契约 | tcp_send 写槽断言 busy==0，把窗口环复用显式化 |
| OBS-NET-2 | P3 | 文档 | seq mod-2^16 无界递增不变量写进 spec |
| OBS-NET-3 | P3 | 文档/假设 | 突发下行依赖 NIC 环 + 快速 recv 的已知假设 |
| OBS-NET-4 | P3 | 加固 | auto_port 冲突审计 + 回绕处理 |

*注：OBS-NET-1 为静态推演的潜在正确性契约缺口，未做动态复现（当前造 Fail 需在部分 ACK 下
拉伸线程），列为 P2 验证性加固建议而非已证缺陷。*