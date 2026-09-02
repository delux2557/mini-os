# 虚拟 TCP 会话协议头规范（netif Step 4，第 1/3 份语义规定）

> 版本 v1.0（2026-09-01）。定义 guest 薄包装与宿主转发器之间、内嵌于 UDP 数据报载荷
> 前部的"会话协议头"，及 `session_id` 生命周期。**动码前定稿；实现与测试以本文件为准。**
> v1.2（2026-09-02）：`MSG_DATA` 的 `flags` 低 16 位复用为**下行序列号 seq**（host→guest），
> 新增 guest→host `MSG_ACK`（0x08）——"薄→厚"第一级台阶：下行可靠停-等。上行可靠 / 滑动窗口
> 为后续候选（见 §6），本次协议头结构不变（GRANT 预留点）。
> **附录 A（2026-09-02）**：补齐跨通道的**传输封装规格**——会话消息在 e1000 / SLIP 两种
> 网卡后端的线上一跳如何封装（谁监听、谁发往谁、SLIP 帧内是完整 IPv4/UDP 数据报）。这是
> 供第三方从头实现兼容转发器 / 配套设施的唯一依据，务必先读附录 A 再读 §2 会话头。

## 1. 载体与字节序

- 载体：`UDP 数据报载荷`（所有虚拟 TCP 流量一律封装成 UDP，走 `netif` → e1000 或 SLIP）。
- 字节序：**网络字节序（大端，big-endian）**，与 IP/UDP 头一致，宿主转发器同规则解析。
- 载荷布局：`[会话协议头(8B) | 载荷(payload)]`；`payload` 即薄包装应用数据。

## 2. 会话协议头字段布局（8 字节定长）

| 偏移 | 长度 | 字段 | 含义 |
|------|------|------|------|
| 0 | 4 | `session_id` | 连接会话标识（uint32，大端），见 §3 |
| 4 | 1 | `msg_type` | 消息类型（DATA / 事件子类），见 §2.1 |
| 5 | 1 | `version` | 协议版本 = **0x01**（v1）；宿主按此分发，预留新旧会话共存 |
| 6 | 2 | `flags` | `MSG_DATA` 状态（host→guest）低 16 位携带**下行数据序列号 seq**（大端）；其余类型恒为 0x0000（v1.2，见 §5）。v1.0 曾恒为 0x0000，本条为该字段的"薄→厚"首次启用 |

`payload` 长度 = UDP 载荷长度 − 8（由 IP/UDP 头给出数据报边界，头内不重复放长度）。

### 2.1 msg_type 取值

| 值 | 常量名 | 方向 | 含义 |
|----|--------|------|------|
| 0x01 | `MSG_DATA` | 双向 | 应用数据（`payload` = 应用字节） |
| 0x02 | `MSG_OPENED` | host→guest（事件） | TCP 连接建立成功 |
| 0x03 | `MSG_CLOSED` | host→guest（事件） | 对端正常关闭（FIN） |
| 0x04 | `MSG_ERROR` | host→guest（事件） | 连接失败 / 对端拒绝（RST/连接被拒） |
| 0x05 | `MSG_TIMEOUT` | host→guest（事件） | 打开/send 超时、半开清理前通知 guest |
| 0x06 | `MSG_OPEN` | guest→host（控制） | **连接请求**：payload = `dst_ip(4B 大端) + dst_port(2B 大端)`，宿主据此对目标发起真实 TCP 连接（见 §2.2） |
| 0x07 | `MSG_CLOSE` | guest→host（控制） | **注销请求**：payload 空；宿主关闭对应 TCP 连接、回收会话表条目（`tcp_close` 用） |
| 0x08 | `MSG_ACK` | guest→host（控制，v1.2） | **累计 ACK**：payload = 下一期望的数据序列号（2B 大端，high then low）。下行可靠停-等专用：转发器收到后知悉"上一报已收、可发下一报"并据此推进 |

**方向说明**：`DATA` 双向；`OPENED/CLOSED/ERROR/TIMEOUT` 均为**事件**（host→guest），是薄包装
失败感知的唯一来源（`msg_type=event` 决定薄包装体验下限）。事件消息 `payload` 可为空。
`OPEN/CLOSE` 为 guest→host **控制**类型，是 guest 主动发起连接/收回连接的唯一入口
（§2.1 增补 v1.1：定稿时 API 契约已引用"向转发器发起 OPEN""发 MSG_CLOSED 注销"，但头表
只列了宿主事件方向；补上后 wire 才能承载 `tcp_open/tcp_close` 的寻址与注销语义）。

### 2.2 连接寻址（`MSG_OPEN` 载荷布局）

`MSG_OPEN` 的 payload 固定 6 字节（不含可空事件载荷规则）：

| 偏移 | 长度 | 字段 | 含义 |
|------|------|------|------|
| 0 | 4 | `dst_ip` | 目标 IP（uint32 **大端**） |
| 4 | 2 | `dst_port` | 目标端口（uint16 **大端**） |

宿主按此对目标发起真实 TCP 连接；寻址**只在 `MSG_OPEN` 中传递一次**，后续 `MSG_DATA`
仅凭 `session_id` 路由，不在每条消息重复带地址（§4「地址无假设」由此成立）。

### 2.3 校验与方向合法性

- 事件类取值（0x02-0x05）**只允许 host→guest**；控制类取值（0x06-0x08，含 v1.2 `MSG_ACK`）
  **只允许 guest→host**；收到方向与取值不匹配的消息（如 guest 收到自身发起的 0x07、host 收到
  0x03）→ 非法，丢弃并告警。
- `DATA` 双向合法。

## 3. session_id 生命周期（三问）

### 3.1 谁分配 —— **guest 分配**（冲突域 = guest 单机）

`session_id` 由 **guest** 在 `tcp_open()` 时生成，宿主转发器**不分配**，只把它当映射表 key。

- 理由：只有 guest 知道"这是一次新连接"；转发器保持无状态、可扩展（§4.1 预留）。
- guest 侧为单调递增计数器（`tcp_open` 时 `++`），32 位足够；回绕可接受（转发器按具体连接工作）。
- **编码为常量**：转发器对 `session_id` 无任何连续性/单调性假设。

### 3.2 编号与回收

- 编号：guest 侧 32 位单调递增计数器；重启后可从随机种子或 0 重新开始，转发器不感知。
- 回收：
  - guest 侧：`tcp_close(fd)` 释放连接对象并 `netif` 发 `MSG_CLOSED` 告知转发器注销该 session（转发器据此关闭对应 TCP 连接、回收会话表条目），计数器**不回退**。
  - 转发器侧：以 `session_id` 为会话表 key；`MSG_CLOSED` 或 idle 超时即回收条目。

### 3.3 guest 崩溃/重启后孤儿会话清理（单列）

转发器无法可靠检测"guest 是否重启"。孤儿清理规约：

1. guest **静默崩溃/重启**（干净场景）：旧会话不再有消息进入 → 转发器按该会话 **idle 超时**
   回收（与半开超时同机制），无需 guest 显式声明。
2. guest **重启后立即重连同一目标**：新 `tcp_open` 生成**新 `session_id`**（计数器重置后新值，
   与旧必不同）→ 转发器视为全新连接，新建会话；旧会话仍等 idle 超时回收。两者 key 不同，
   **不会误复用/串线**。
3. 若 guest 崩溃瞬间存在半开 TCP：由转发器 **TCP 侧**超时/复位处理（真 TCP 状态机只在宿主），
   guest 侧无感知。

> 结论：孤儿清理**统一走"转发器 idle 超时"**，不引入 guest-restart 显式握手协议；
> 简单、健壮，且不破坏"转发器无状态、可扩展"的架构预留。

## 4. 地址无假设（实现期约束 5 的文档落点）

会话协议头 **不含任何 IP/端口/地址字段**，会话仅由 `session_id` 标识。串口（SLIP）模式下
IP 是**名义地址**（实测 DHCP 回落静态 10.0.2.15、宿主对端可冒充 10.0.2.2），转发器与 guest
的会话协议一律**不对地址作假设**；目标寻址在 `tcp_open` 的调用参数传递、由各网卡适配层完成。
调试串口 demo 时勿被名义 IP 绕路。

## 5. 校验与边界

- 转发器解析头时校验 `version==0x01`，否则丢弃并计告警（新旧会话按此分发）。
- 头不足 8 字节的 UDP 载荷 → 非法，丢弃（host / guest 双侧一致）。
- `flags`：v1.2 起，`MSG_DATA`（host→guest）低 16 位携带下行数据序列号 seq（任意值合法）；
  其余类型 `flags` 恒为 0x0000（非法高位 → 丢弃）。解析方按 `msg_type` + 方向决定是否提取 seq。

## 6. 可靠性语义（v1.2，下行靠停-等；上行可靠/滑动窗口为候选）

### 6.1 下行可靠（已落地，host→guest）

> 动机：v1.1 把 `TCP_RXB` 提到 4096 只"抬高丢尾阈值"，未消掉 NIC/socket 界面 burst 随机丢包的
> 根因（>16KB 依旧缺尾，见 bugs.md BUG-047 收尾）。下行改**可靠停-等**后，缺尾从根子消除。

- 每个 host→guest `MSG_DATA` 的 `flags` 低 16 位 = 递增 `seq`（从 0 起，每会话累计）。
- guest 侧：只接受 `seq == 下一期望(rx_next)` 的顺序包，推入 rxb 并回 `MSG_ACK`
  （payload = `rx_next+1`，2B 大端）；重复/乱序包（ACK 丢后重发）**幂等丢弃载荷**并重发 ACK。
- 转发器侧：每会话 **恒 ≤1 报在途**（in-flight），收到累计 ACK 才发下一个；in-flight 的 ACK
  超时按 `RETX_MS=2.0s` 定时原样重发（`_retransmit`）。SLIP 慢通道单报回环 ~1s，重传间隔须 ≥ 此值
  （60ms 会灌爆慢 UART）；e1000 快通道回环 <10ms，正常无丢不触发重传，2s 只在真丢时恢复。
- 语义等价：下行变成**可靠字节流**（暂仍按报分块，应用侧 tcp_recv 感知不到重传）。这是
  "薄→厚"第一级台阶——guest 开始参与 seq/ACK，但**无发送缓冲/重传定时器/窗口，非完整状态机**。

### 6.2 上行可靠（候选，未动码，guest→host）

下行已可靠；上行仍**尽力而为**（guest→host `MSG_DATA` 转发器收到即转发）。把停-等**对称**搬到
上行：guest 发送方向保留发送缓冲 + 分配 seq、转发器回 host→guest ACK、guest 加重传定时器。
wire 增补 host→guest ACK 控制类（或复用 flags 占位），与下行对称；薄包装 API / 会话表 / 头结构
不破坏（对应 roadmap"虚拟 TCP 薄→厚演进候选·上行可靠"）。

### 6.3 滑动窗口（候选，未动码，性能项）

停-等"发 1 等 ACK"链路上恒 ≤1 报在飞，慢通道带宽亏一半以上。允许 in-flight=窗口 W（≥1）：
时序维持累计 ACK、仅重传丢的那一报 → 吞吐随窗口线性提升。seq 机制已具备；窗口上限由 L2/MTU 与
两端缓冲决定，初期取保守小值（如 4），"先跑通再优化"（对应 roadmap"…演进候选·滑动窗口"）。
与上行可靠正交、可独立推进。

## 附录 A：传输封装规格（第三方实现指南，2026-09-02）

> 正文 §1-§6 描述的是**会话层**：一条虚拟 TCP 会话 = 一条**会话消息**（8B 会话头 + 载荷）。
> 但会话消息在链路上怎么走，决定第三方能否写出可互操作的转发器。本附录补齐"线上这一跳"。
> 一句话：**两台网卡后端（e1000 / SLIP）的会话消息都是同一个字节串**，只是到达转发器时被
> 包了一层不同的 IP/UDP 封装。转发器对两种后端做**同一套会话处理**，只在"收发帧"处接入。
> 参考实现：`tests/tcp_proxy.py`（`run_udp` / `run_slip` + `SlipDecoder` / `udp_session` / `make_udp`）。

### A.1 会话消息 = 跨通道统一的"载荷单元"

无论哪种后端，转发器眼里都是**一段会话消息** `会话头(8B) + payload`：

| 消息类型 | 编号 | 载荷（payload） |
|---|---|---|
| `MSG_DATA`（下行 host→guest） | 0x01 | flags 低 16 位 = 下行 seq；载荷 = 应用字节 |
| `MSG_DATA`（上行 guest→host） | 0x01 | 载荷 = 应用字节 |
| `MSG_ACK`（guest→host） | 0x08 | 下一期望 seq（2B 大端） |
| `MSG_OPEN` / `MSG_CLOSE` | 0x06 / 0x07 | 6B 目标寻址 / 空 |
| 事件 `OPENED/CLOSED/ERROR/TIMEOUT` | 0x02-0x05 | 载荷（多为空） |

> **基本盘**：会话消息本身**不再被任何东西再封装一层自定义协议**。两种通道只做"IP/UDP 打包 ±
> SLIP 成帧"，不会再往里加私有 magic。第三方若要写转发器，只需：识别会话消息 → 按 §2-§3
> 解析 → 按 §6 推进可靠传输。

### A.2 e1000 通道（UDP socket，udp 模式）

- **端点**：guest 用**单条 UDP socket**（懒创建，`src/app/tcp.c`），发往转发器
  `10.0.2.2:7778`（`TCP_PROXY_IP=10.0.2.2`、`TCP_PROXY_PORT=7778`，QEMU/SLIRP 网关视角下即宿主）。
- **封装**：**会话消息整体直接作为 UDP 数据报载荷**——无再封装。UDP 源端口由 guest 自动分配，
  转发器以 `(guest_ip, guest_udp_src_port)` 作为回传目的地址（`Session.addr`）。
- **转发器侧**：`run_udp(port=7778)` 绑 `127.0.0.1:7778`；收包后 `parse_hdr(pkt)` 直接解析
  （`pkt[0:8]` = 会话头，`pkt[8:]` = 载荷）。回传 `peer_send(addr, 会话消息)` → 原样 UDP sendto。
- **第三方要点**：绑定 UDP 端口 **7778**；每个收包 = 一条完整会话消息；回包目的 = 收包的源地址。

### A.3 SLIP 通道（COM2 串口，slip 模式）

- **端点**：转发器连到 **QEMU COM2 的 TCP 串口对端**（`--host 127.0.0.1 --port 7901`），
  上层是 SLIP 流；转发器用 `SlipDecoder` 从字节流里拆出 SLIP 帧。
- **封装（关键，最易踩坑）**：SLIP 帧里装的**不是**裸会话消息，而是**完整 IPv4 数据报**——
  `[SLIP 帧] → [IPv4 头] → [UDP 头] → [会话消息]`。理由见 roadmap D1：netif 包单位 = **IP 数据报**
  （串口无以太网语义，SLIP 就是 "IP over 串口"）。第三方转发器必须做 **IP+UDP 的解析与反向组帧**。
- **解析**：`SlipDecoder.feed` 拆帧后，`udp_session(frame)` 剥 IPv4+UDP，得到
  `(sip, sport, dip, dport, payload)`，其中 `payload` = 会话消息。SLIP 成帧按 **RFC 1055**
  （END `0xC0` / ESC `0xDB` / ESC-END `0xDC` / ESC-ESC `0xDD`）。
- **回传（源/目的对调）**：`peer_send` 收到的是四元组 `(sip, sport, dip, dport)`（guest 视角），
  回包要**交换源/目的**，用 `make_udp(dip, dport, sip, sport, 会话消息)` 重建 IPv4+UDP（含
  **UDP 校验和与 IPv4 伪头**，RFC 768/1071），再 SLIP 成帧写回串口。
- **第三方要点**：串口上看到的是 IPv4 数据报流；必须做 IP/UDP 组帧与校验和；源/目的 IP 与端口
  在应答方向对调。SLIP 收发都用 RFC 1055。以下是转发器侧的装箱对照：

| 方向 | 处理流程 |
|---|---|
| guest→host（收） | SLIP 拆帧 → `udp_session` 解析 IPv4/UDP → 得到会话消息 → `handle_msg` |
| host→guest（发） | 构造会话消息 → `make_udp` 组 IPv4/UDP（源=原目的、目的=原源）→ SLIP 成帧 → 写串口 |

### A.4 与"会话头地址无关"原则的关系

会话头本身**不含任何 IP/端口**（§4）。SLIP 模式下的 IP 是**名义地址**（guest 常为 10.0.2.15、
对端可冒充 10.0.2.2）；转发器**不假设地址语义**，只把四元组当"往返目的地址"看待（收什么、回
给谁，原样对调）。因此第三方实现无论跑在哪个真实地址上都不影响会话互操作。

### A.5 最小互操作清单（第三方写转发器前的自查）

1. 绑定/连接正确的端点：e1000 → UDP `:7778`；SLIP → QEMU COM2 的 TCP 串口。
2. 从通道里取出"完整会话消息"：e1000 直接是 UDP 载荷；SLIP 需先拆 RFC1055 帧、再剥 IPv4/UDP。
3. 解析 `8B 会话头`（§2）：session_id / msg_type / version / flags，字节序大端。
4. 按 msg_type 建会话、路由：OPEN→发起真 TCP；DATA→upstream 转发 / downstream 回传；
   ACK→推进下行；OPENED/CLOSED/ERROR/TIMEOUT→回传事件。
5. 可靠下行（§6.1）：下行 DATA 带 seq，in-flight ≤1，收到 guest `MSG_ACK` 才发下一个，
   超时 `RETX_MS=2.0s` 重传。
6. 注意 MTU：单条会话消息（含 8B 头）≤1400B（见 `tcp-mtu-fail.md`）。

> 若以上 6 步与参考实现 `tcp_proxy.py` 行为一致，即视为互操作达成；验证用 `tests/test_tcp_dl.sh`
> / `test_tcp.sh`（双通道 200 OK + 尾字节完整）。