# 虚拟 TCP 薄包装 API 契约表（netif Step 4，第 2/3 份语义规定）

> 版本 v1.0（2026-09-01）。**动码前定稿。** 薄包装 = "API 形状 + 事件通道" 先立起来，
> 厚包装在既有对象与通道里补状态机（不改签名、不破帧格式）。
> v1.1 注记：正文结构体里的 `pid`/`txq`/`timeout_tick` 字段在**用户态 per-process** 实现里
> 未单列——进程天然隔离（省 pid）、薄包装一次 send 即发一条报文（`txq` 留作厚包装流式拼接
> 预留、暂为空）、recv 超时上限由编译期常量 `TCP_RECV_TICKS` 承担（非字段）。`tcp_close` 的
> 注销在 wire 上发的是 **guest→host 控制类 `MSG_CLOSE`**（非 host→guest 的 `MSG_CLOSED` 事件），
> 方向语义见 `tcp-session-proto.md` §2.1。
> v1.2 注记（2026-09-02）：下行可靠停-等（接收侧 `rx_next`）+ **上行可靠停-等**（发送侧
> `tx_inflight` 副本 / `tx_seq` / 阻塞等 ACK）实现落地，均为对象内状态、**不改 API 签名**（send 由
> "一发即返回"升级为"确收后返回 `n`"，仍返回字节数；见 §1）。
> v1.3 注记（2026-09-02）：**上行改滑动窗口**——`tcp_send` 把载荷写入发送窗口（`TCP_TXWIN` 槽×独立
> seq），**返回 `n` 即入窗已发出**（不再阻塞等单报确收），窗口满才让步等累计 ACK 推进；重传以窗口最老
> 未确认槽为单位。send 的互斥语义仍成立（本地失败-1 / 成功返回字节数），只是"确收"时点由"单报 ACK"
> 前移至"入窗发出"，可靠性由窗口重传兜底。「下行滑动窗口」候选仍只加状态、不破接口（见
> roadmap"薄→厚演进候选"）。

## 1. 返回语义总表

| API | 签名 | 返回 | 精确语义 |
|-----|------|------|----------|
| open | `int tcp_open(uint32_t ip, uint16_t port)` | `>=0` fd | 连接对象就绪；已分配 session_id、构建连接对象、向转发器发起 OPEN（非阻塞，真实握手在宿主异步完成） |
| | | `-1` | 本地失败：资源不足 / 参数非法（ip 或 port 为 0）/ 单包寻址无法路由到可用网卡 |
| send | `int tcp_send(int fd, const uint8_t* d, uint32_t n)` | `n` | **v1.3 起为"入窗即发出"的滑窗可靠发送**：载荷写入发送窗口（`TCP_TXWIN` 槽×独立上行 seq）并立即按序发出，返回 `n` 即已入窗**且发往转发器**；窗口满则阻塞让步等累计 ACK（host→guest `MSG_ACK`）推进窗位后继续，期间最老未确认槽超时重传。大文件上传=循环多次 `tcp_send`，按窗口流水线扇出、无需逐包等 ACK。载荷 ≤ 单包上限（见 MTU 规约） |
| | | `-1` | 本地失败：无效 fd / 连接未建立 / **超单包上限** / 连接已关闭或处于失败态——一律本地立即拒绝，**不发报文、不依赖转发器回事件** |
| recv | `int tcp_recv(int fd, uint8_t* buf, uint32_t max)` | `>0` | 收到 n 字节应用数据（DATA） |
| | | `0` | **对端正常关闭**（收到 `MSG_CLOSED`）——与失败必须可区分 |
| | | `-1` | **失败或超时**（收到 `MSG_ERROR` / `MSG_TIMEOUT`，或内部错误 / 无效 fd）——与"对端关闭"语义互斥，二者恒可区分 |
| close | `int tcp_close(int fd)` | `0` | 连接对象已销毁、session_id 已归还（发控制类 `MSG_CLOSE` 通知转发器注销；非 `MSG_CLOSED` 事件） |
| | | `-1` | 无效 fd / 已关闭 |

### 1.1 recv 的阻塞模型（关键）

`tcp_recv` 为**有超时上限的阻塞等待**（对齐"HTTP 请求-响应"一次 send 一次 recv）：
内部在 `sys_sleep`（tick）间轮询事件队列，直到出现下面之一即返回，避免"暂无数据"歧义：

- `DATA` → 追加到接收缓冲 → 返回字节数（`>0`）；
- `CLOSED` → 返回 `0`；
- `ERROR` / `TIMEOUT` → 返回 `-1`；
- 达到内部超时上限（编译期固定，如 5s）而仍无结果 → 返回 `-1`（防挂死）。

> 此模型保证返回值三态（`>0`/`0`/`-1`）互斥且完备，`0`（正常关闭）与 `-1`（失败/超时）
> 语义永不被"暂无数据"污染。

## 2. 连接对象结构体（guest 侧，fd → 对象映射，非裸整数）

```c
typedef enum { TCP_IDLE, TCP_OPENING, TCP_OPEN, TCP_CLOSED, TCP_ERROR } tcp_state_t;

typedef struct tcp_conn {
    int          used;          /* 槽被占用 */
    uint32_t     pid;           /* 归属进程（v0.31 同款资源归属） */
    uint32_t     session_id;    /* 会话标识（guest 分配，§1 spec） */
    tcp_state_t  state;         /* OPENING -> OPEN -> CLOSED/ERROR */
    uint32_t     dst_ip;        /* open 目标（仅 guest 侧语义记录；会话协议不含地址） */
    uint16_t     dst_port;      /* 同上 */
    /* 流式缓冲（§4.1 预留点 2）：薄包装一次 send=一条报文，结构按流式设计可拼接/分段 */
    uint8_t      txq[TCP_TXQ];  uint16_t tx_head, tx_tail;
    uint8_t      rxb[TCP_RXB];  uint16_t rx_head, rx_tail, rx_len;
    /* 事件队列（§4.1 预留点 3：控制/数据通道分离） */
    struct { uint8_t type; uint16_t len; uint8_t data[TCP_EVT]; } ev[TCP_EVQ];
    uint16_t     ev_head, ev_tail;
    uint8_t      ev_overflow;   /* 队列满曾丢弃 DATA 数据 的标志 */
    uint32_t     timeout_tick;  /* recv 超时上限（tick 计） */
} tcp_conn_t;
```

## 3. 事件队列满时的行为（写死）

事件队列为环（容量 `TCP_EVQ`）。**满时的写入规则**：

1. **数据事件 `DATA` 可丢弃**：满时丢弃最旧 `DATA`（数据可重发/可被超时兜底），并置
   `ev_overflow=1`；不阻塞上层、不让写入方等待。
2. **状态事件 `OPENED/CLOSED/ERROR/TIMEOUT` 绝不丢弃**：它们是 recv/本次会话**返回值的
   决定性来源**；若满，则**覆盖最旧的 `DATA`**（必要时连覆盖多枚 DATA）腾位写入状态事件，
   且置 `ev_overflow=1`。
3. 事件被丢弃时，`recv` 侧以超时（`-1`）兜底返回，绝不静默吞"关闭/失败"语义。

> 效果：状态语义（关闭/失败/超时）在队列压力下**单调保持**，数据可以被牺牲。

## 4. fd 映射

`tcp_fd` = 连接对象数组 `tcp_conn_t[TCP_CONN_MAX]` 的下标（0..N-1），封装成 fd 整数供应用使用。
所有 API 先校验 `used && pid==当前进程` 再操作（沿用 per-process 资源归属，禁用跨进程乱关）。