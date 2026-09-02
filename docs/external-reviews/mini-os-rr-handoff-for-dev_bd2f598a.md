# mini-os record/replay 基础设施实测 — 问题交接单（含 dev 核实与处置批注）

> **来源**：外部评审方（审核）提供 · **原始报告时间戳**：2026-09-02
> **实测对象**：main `1d62783`（Merge pull request #28 from delux2557/fix/ci-comment-layer-count，评审方标注 v1.4.8）
> **复核注**：该对象是当前 main（`36bd085`，含 #29/#30/#31）的**祖先**；#30/#31 未改动 `transcript.sh`/`replay.sh` 的 ack/背压机制、`httpdemo.c`/`dldemo.c` 的端口硬编码，故下述 F1/F2/F3 判断在**当前** main 上仍然成立。
> **归档**：按 external-reviews/README 惯例，原文原样归档，末尾附 dev 复核批注（不替代原文）。

---

- [对象与边界](#对象与边界)
- [0. 一页摘要](#0-一页摘要)
- [F1 · 回放器无输入背压](#f1p1回放器无输入背压--多行命令流回放必现吞行并线)
- [F2 · 录制侧 tr_send 同样无 ack](#f2p2transcript-录制侧-tr_send-同样无-ack--慢宿主下-golden-自身残缺)
- [F3 · HTTP 端口覆盖不贯穿](#f3p2-http-端口覆盖不贯穿http_port-只改宿主侧guest-编译期硬编码--假阳性)
- [F4 · golden 无窗口概念](#f4p3golden-无窗口概念boot-自动-demo-输出与命令流混排跨轮判据天然脆)
- [F5 · 网络注入流无格式](#f5能力缺口transcript-只覆盖串口通道网络注入流无格式)
- [F6 · 注记](#f6注记非项目问题)
- [内核侧结论](#内核侧结论正面记录供放心使用)
- [地基价值判定](#地基价值判定)
- [dev 核实批注与处置](#dev-核实批注与处置)

## 对象与边界

对象：main `1d627835`（v1.4.8）· 环境 Debian 13 / gcc 14(-m32) / nasm / Python 3.13 / QEMU 10.0.11（仅 TCG，无 KVM）

方法：以业界「录失败现场 → 修复版回放差分 → 可用同环境重放复验」为准，用项目自身地基（`tests/transcript.sh` 录制 / `tests/replay.sh` 回放 / `[kb] readline` 等内核审计行）跑压力、攻击、bug 探查。

边界承诺：本轮未向远端做任何写操作，工作树保持净 main（git status 无改动，产物只在 `build/`）。

## 0. 一页摘要

| 维度 | 结论 |
|---|---|
| 内核防御面 | **未击穿**。syscall 越权 17 项、shell 输入攻击、cc500 历史引信 4 类、坏 ELF、crash/stackovf、SLIP PHY 501KB 畸形帧、~4000 虚拟 TCP 脏包：全部拒止/降级，0 panic。 |
| 回归资产 | 账本 50 项修复全部有效（BUG-026/031/032/039/040/048/049 复测未复现）；128KB(8×MTU) 下载完整；make test 12 层全绿。 |
| test-det（v1.4.8 修复） | 本地 4 连绿，串行+-nic none+确定性前缀方法论成立。 |
| record/replay 地基 | 发现 1 个 **P1 级工具链缺陷**（回放器无输入背压，实锤）+ 3 个 P2/P3 改进项。修好前，「回放旧 transcript 复现失败」对多行命令流**不可信**。 |

严重度排序：**F1**（阻塞地基采用）> **F2**（录制侧同类）> **F3**（测试可移植性）> **F4**（判据粒度）。F5 为能力缺口（非缺陷），F6 为使用注记。

---

## F1【P1】回放器无输入背压 → 多行命令流回放必现吞行/并线

### 表现

同一份 transcript（录制侧干净），P3 回放后 guest 执行的命令数少于录制：

最小实验（6 writefile + 6 rm + ls + selftest，in.tr 共 14 条）：

| 轮次 | `[shell] rm '/xN.c'` 实际执行 | ls 残留文件 |
|---|---|---|
| 录制 golden（逐行 ack） | 6/6 | 0 |
| 回放 #1（`REPLAY_ICOUNT=1 replay_into`） | 5/6 | `x6.c`×2 |
| 回放 #2 | 3/6 | `x2.c x5.c x6.c` |

压力域两例同根因（现场更完整）：

- S1（30 条命令）回放：`rm /s3.c /s4.c /s5.c` 三行丢失，日志出现 `unknown command: 5.c`，ls 见 s3–s5 残留；
- S2（heredoc 11 行源码）回放：吞 2 行（golden `wrote 52 bytes` → 回放 `30 bytes`），ccrun 结果 code=0 PASS → 4294967295 FAIL。

### 定位（机制证据）

in.tr 相邻命令 delta 抽样：`32/35/36/37/59 ms`（与内核打印节奏一致，天然小间隔）。

`replay_into`（tests/replay.sh）消费 `rel_ms` 做 sleep delta 打拍，**每轮只 sleep，不等上一行被 guest 消费**（唯一的等待是开局 prompt 硬下限与结尾 `done_regex`；行级无任何 ack/下限钳制）。慢速消费方（icount+TCG 无 KVM、shell 输出中、后台 demo 抢占）下，多行字节淤积在串口路径上跨行合并 → 前缀被上一行残渣吃掉（`5.c` 形态）或整行静默丢弃 → 回放轮与录制轮**不等价**。

guest 侧承接历史同类：kb 行缓冲 `line_ready` 期间追加 = **BUG-034 同一风险域**（rec-ack 缺失下由工具链侧重新触发）。

### 复现（净 main，~10 分钟）

```
git checkout 1d627835 && cd v2-c-kernel && make
# 录制（关键点：逐行等 `[kb] readline` 行出现再发下一条，保证 golden 干净）
#   命令流：mini-os$ 出现后依次发 writefile /x1.c ... /x6.c（逐条 ack）、
#            rm /x1.c ... /x6.c（逐条 ack）、ls ; selftest ; 等 "[selftest] audit="
#   产物：build/transcripts/rp-min/{in.tr,out.tr}
#   预期 golden：rm 行 6、ls 残留 0
. tests/replay.sh
REPLAY_ICOUNT=1 replay_into build/transcripts/rp-min/in.tr /tmp/rp_test.log runR
grep -ac "rm '/x" /tmp/rp_test.log    # 预期 6，实测 3~5（跨进程/宿主复现率 100%）
grep -a 'unknown command' /tmp/rp_test.log
```

### 修复方向（任一即可闭环，建议 a）

- **a)** 回放器行级 ack 节拍：消费 in.tr 时不等墙钟 delta，改为「等上一行在 out_log 产生 `[kb] readline pid=` 新行再注入下一行」（delta 只留审计价值）。录制侧顺带把 `rel_ms` 改记 ack 时刻，让 in.tr 本身就是"消费时间轴"。
- **b)** 保守版：`replay_into` 每行 `sleep max(delta, ACK_INTERVAL_MS)` + readline 计数门（双保险）。
- **c)** guest 加固（并线根因侧）：kb 行缓冲在 `line_ready` 消费完毕前拒收下一行字节（防合并），与 QEMU 侧串口 FIFO 反压配合。

### 影响判定（给结论校准用）

- 官方 `test_replay.sh` 因此当前是**安全**的：触发输入为单行 `writefile /bug026.c int main(int x` + ccrun，行少不触发；且其注释已自认跨轮一致性只能"尽力检查 non-gate"——本缺陷正是"尽力"盖不住的那块。
- 该缺陷与 transcript.sh 头注释的规划口径（"相对 ms，host 墙钟；真 icount 虚拟时钟留 P3"）相符：时钟语义迁移本就排在 P3 未完成项，此为按计划的补全点，不是设计事故。
- 「录失败→修版本必须回放仍红（同缺陷签名）→再修绿才关单」的回归价值已验证成立——前提是修完本项：用同版本内核，golden 与 replay 的结论型里程碑（selftest / cc500 error 行 / writefile 字节数）语义一致；残留差异全部是时钟量，可白名单归一。

## F2【P2】transcript 录制侧 tr_send 同样无 ack → 慢宿主下 golden 自身残缺

### 表现

实测于裸 QEMU 无 icount 录制轮：命令流 `hello / help / ls / selftest`（tr_send 固定 0.4s 节拍）：golden 里 selftest **整条丢失**（`unknown command: hello` 在、selftest 输出零行），同 in.tr 用 icount 回放反而 4 条全执行——录制轮比回放轮更不可信，"录旧版失败现场"在源头失真。

### 机制

同 F1：0.4s 裸注入 vs `help` 数十行输出未完成时，输入跨行合并/覆盖（kb 缓冲在「提示未就绪 / 演示输出中」窗口内合并，**BUG-034 同族**）。

### 修复

`tr_send` 注入后等同步确认 `[kb] readline` 自增再返回（out_log 由调用方共享 fd 可及）；超时未消费 → 报 `error: input ack timeout` exit 2（与 harness 退出码语义统一规范对齐：0/1/2 = ok/断言 fail/环境病）。

**附带建议（消费侧共性需求）**：in.tr 记 ack 时刻为 `rel_ms`（理由同上）。

## F3【P2】HTTP 端口覆盖不贯穿：HTTP_PORT 只改宿主侧，guest 编译期硬编码 → 假阳性

### 表现

`tests/test_tcp_attack.sh` 注释宣称"端口用环境变量覆盖，避免 CI 冲突"，但：
- `src/app/httpdemo.c:13 #define HTTP_PORT 8080`、`src/app/dldemo.c:15 DL_PORT 8080`（**guest 编译期**）
- 宿主 server / curl 自检 / proxy --target 跟随 `$HTTP_PORT`
- 宿主 8080 被占的场景（共享 runner 等均成立）：server 起不来 → 业务断言 5 连 `[FAIL]`，内核实际无恙（同场景改端口后全绿）；guest 请求打到错误端口时 proxy 日志出现 `MSG_OPEN -> TCP 127.0.0.1:18080 / open fail: Connection refused` 形态——"业务链路失败"完全由环境决定，易误读为"攻击击穿 TCP 栈"（本轮真实走过一轮该误判，值得写进教训）。

净环境复核：原生 8080 + `unshare -rn` 下，S5 攻击 3956 包 / 15s、业务 8KB+TAIL 完整交付 = 真 `[PASS]`（脚本 exit 1 语义正确，先前"rc=0"为管道误采，已更正）。

### 复现

任意 8080 被占的 Linux 上：`bash tests/test_tcp_attack.sh` → Part A/B 业务断言 5 FAIL（而内核存活标记检查 `[ok]` 内核无异常标记同时成立——矛盾即信号）。

### 修复选项（推荐 v1）

- **v1 脚本侧防御（最低成本）**：`run_http_server` 前 `ss -ltn | grep -q ":$HTTP_PORT "` 且 bind 测试失败 → exit 2（环境病），与既有退出码规范/BUG-043/046 治理一致——"环境病"与"代码病"必须可区分，这正是该仓自己立的标准。
- **v2 真贯穿**：`Makefile HTTP_PORT ?= 8080` → `-DHTTP_PORT=$(HTTP_PORT)`，脚本单点传参，curl/断言/proxy/guest 四处单一来源。

（附）同型先例：roadmap v1.4.7 已记录 repro_bugs.sh 的 `wait_for "shell 提示符"` 8s 在慢宿主假红并改为"就绪信号 + 放宽超时"——同一治理口径，建议本项对齐。

## F4【P3】golden 无"窗口"概念：boot 自动 demo 输出与命令流混排，跨轮判据天然脆

### 表现（均为观测事实，非缺陷，但每个消费方都要重新踩）

- boot 期 tick/调度行在 icount 轮与非 icount 轮行数不同（观测样本：boot 后 36 tick vs 21 tick）；tick 量本身两轮一致（内核虚拟时钟确定性 OK）。纯命令窗口（writefile 字节数、selftest 结论行）跨轮逐字节一致——地基可判性成立。
- `[audit] mem ok: frames used=462→465`、`sched ok: 5 alive→6 alive` 等**状态计数行**跨轮漂移（回放/重录都有，属活体快照量）。
- boot 自动 demo（forkdemo 父子交错、FA 写文件行）与 transcript 窗口重叠：不加窗口锚点时，golden vs replay 的**首个差异永远是 boot 行**。

### 修复方向（下沉到 transcript 层，避免各消费方造轮子）

- **tr_mark**：显式开窗锚点 API（如 `tr_mark "$LOG" "type 'help' for commands"`，out.tr 只存锚后内容 + 记录锚 seq）——地基从"原始字节流"升级为"逻辑会话"。
- 或消费侧约定：判据先 awk 截到 shell 就绪锚后，再按行 `s/[0-9]+/N/g` 归一化比对（本轮 harness 实测有效，可当现成参考实现）。
- tick/audit/sched 计数行列入判据白名单外（或做集合比对而非序列比对）。

## F5【能力缺口】transcript 只覆盖串口通道；网络注入流无格式

本轮把 SLIP PHY 攻击（7 模板×600 帧/501KB，seed=13579，含 END 洪水/悬挂 ESC/巨帧截断/坏 IP 头/连珠帧/纯随机）以 in.tr 同构 `seq\trel_ms\tb64(payload)` 记录并原节奏回灌验证：内核 0 panic、调度持续、攻击流可重放——格式天然可承载网络输入，整机回放缺这一半。建议定义 **net.in.tr 姊妹流** + 回放器按 rel_ms 重放到 peer/proxy socket（proxy 侧加"已转发"计数行作 ack）。

附带 SLIP 健壮性证据（正面）：畸形帧含超长残帧悬挂时，guest 靠 idle/END 恢复自同步，事后正常回包（本轮 600 帧攻击流中存活）。

## F6【注记，非项目问题】

- 攻击/压力套件退出码语义正确（exit 1 上抛），与 harness 统一规范（0/1/2）一致——先前"ATTACK_RC=0"为 PIPESTATUS 误采，更正。
- 128KB 下载在 icount+TCG 下整轮 ~2355s 量级（无 icount 数百秒）：test-det 类 gate **不要叠加整机下载**；复现时钟敏感性须用 icount shift 精确口径（v1.4.8 已按 -nic none+前缀正确绕开）。
- `repro_bugs.sh` 的 `exec /out.elf /cc500.c /out2.elf` 断言是"未复现即通过"的软分支（argv 语义回归时不会红）；建议账本加"argv 生效"专项断言。

## 内核侧结论（正面记录，供放心使用）

| 攻击面 | 载荷 | 结果 |
|---|---|---|
| syscall 越权 | run abuse（17 项内核低/回绕地址指针） | 全部 rejected(-1)，无 ACCEPTED |
| 内存隔离 | run crash/stackovf/isol | guard 击杀+audit，内核/他进程无恙 |
| shell/kb 输入 | 600B 超长行、heredoc 变体、rm /..、mkdir //、空参、不存在路径 | 无合并崩溃、无注入逃逸，selftest 6/6 收尾 |
| cc500 引信 | 未闭合字符串/块注释/123abc/未定义函数 | 全 `cc500: error` 拒止，无死循环（048/049 域） |
| ELF 解析 | writefile 垃圾文件→exec | 拒绝加载，存活 |
| SLIP PHY | 501KB 畸形帧（7 模板，固定 seed）+ 回灌复验 | 0 panic，通道自同步恢复 |
| 虚拟 TCP | 3956 脏包 3 线程 15s（净环境） | 业务 8KB+TAIL 完整交付，无副作用 |
| 大文件 | 128KB 下载（8×MTU） | 尾字节+EOFTAIL 完整 |

## 地基价值判定

地基有作用：无 golden/replay 差分与里程碑白名单，F1/F2 会被读成"压测偶发红"——本轮正是用 P1(icount)+P2[transcript]+P3[replay] 把工具链缺陷钉死到 14 条命令 × 32–59ms delta 的精度；「录失败→修复仍须红→回归关单」的机制本身成立。

待补强（优先序）：
- **F1/F2** 输入背压（同一机制可一并解决：send 后等 `[kb] readline` 自增做 ack；录制记 ack 时刻，回放按 ack 节拍重放——顺带天然对齐 rel_ms 时钟语义）；
- **F4** 开窗/归一化（tr_mark，把"逻辑会话"变成地基原语）；
- **F5** 网络流姊妹格式 + 回放器支持（P3 的"整机"最后一块）。

内核可观测性设计（`[kb] readline`/audit/selftest 收敛行）是地基最顺手的现成抓手：**修复全部可落在 `tests/` 三文件（transcript/replay/各套件头部防御），`src/` 零改动**——地基缺陷不需要动被测对象来治。

---

## dev 核实批注与处置

> 复核人：dev（本机 decode 侧核查）· 时间：2026-09-02
> 复核方式：源码级核查机制断言（不重复重放巨量攻击——成本高；攻击结果数据采纳原始报告）。

### 对象与版本核实

- 交接单对象 `1d627835` == 本地 `1d62783`（Merge pull request #28），确为当前 main `36bd085` 的**祖先**。评审方端标注 v1.4.8；当前 main 已含其后 #29/#30/#31。
- 关键：后三次合并**未改动** F1/F2 所述 `transcript.sh`/`replay.sh` 机制、也**未改动** F3 所述端口硬编码 → 下述判断当前仍成立。

### 逐项核实结论

| ID | 断言 | 复核（src 证据） | 结论 |
|---|---|---|---|
| **F1** | 回放器仅按 `delta` sleep、无行级 ack/背压 | [replay.sh L43-L52](v2-c-kernel/tests/replay.sh#L43-L52)：`delta=$((rel-prev)); sleep…` 逐行打拍，唯一等待是开局 prompt 硬下限(L40)与结尾 done_regex(L62)，**行级无确认钳制** | ✅ **属实**（P1，当前 main 同现） |
| **F2** | `tr_send` 注入后不待 ack、rel_ms 记 host 墙钟 | [transcript.sh L60-L70](v2-c-kernel/tests/transcript.sh#L60-L70)：`printf >&9` 后直接 `tr_emit_in` 返回；L26/L32-34 rel_ms 为 `date +%s%3N` 墙钟 | ✅ **属实**（P2） |
| **F3** | guest 编译期硬编码 8080、宿主变量不贯穿 | [httpdemo.c:13](v2-c-kernel/src/app/httpdemo.c#L13)/[dldemo.c:15](v2-c-kernel/src/app/dldemo.c#L15) `#define HTTP_PORT 8080`；[test_tcp_attack.sh L14/L48-L49](v2-c-kernel/tests/test_tcp_attack.sh#L14) 宿主侧控制且强制回 8080 匹配硬编码 | ✅ **属实**（P2） |
| **F4/F5** | 观测事实 / 能力缺口 | 与 transcript 现产物规约（原始字节 + in/out 两流）一致，属地基设计边界 | ⚠️ **采纳为待补强项**（非缺陷） |
| **F6** | 退出码 0/1/2 = ok/断言/环境 | 仓库多套 harness（test_net/test_tcp/test_persist/test_serial/test_transcript）头注释一致声明 `exit 0=全绿 / 1=断言失败 / 2=环境或依赖缺失（避免环境病伪装成代码病）` | ✅ **属实**（与仓库自定规范一致；F3-v1 建议的 exit 2 正对齐此规范） |

### 处置建议（本交接单登记，另行开工/拆 PR，不随归档合入代码）

1. **F1（阻塞，最高优先）**：`replay.sh` `replay_into` 由 `delta sleep` 改「等上一行产生 `[kb] readline pid=` 新行再注入下一行」的行级 ack 节拍；`delta` 保留为审计/涨停。**单一来源提醒**：本项目有 arena gate 复用基线（阶段0-2），此改造须让 replay 输出仍满足契约行构造，避免误伤已合入的 baseline/evaluate 判定。
2. **F2（同批）**：`transcript.sh` `tr_send` 注入后等 `[kb] readline` 自增 ack，超时 `exit 2`（环境病）。与 F4 的"记录 ack 时刻为 rel_ms"一并做——in.tr 即消费时间轴。
3. **F3（低成本即可上）**：`test_tcp_attack.sh` `run_http_server` 前端口占用检测 → 命中 `exit 2`（环境病，不误报"攻击击穿 TCP"）；后续可走 v2 真贯穿（Makefile `-DHTTP_PORT`）。
4. **F4**：tr_mark 开窗锚点下沉 transcript 层（把"逻辑会话"做成地基原语），判据统一在就绪锚后归一化。
5. **F5**：net.in.tr 姊妹流 + replay 到 peer/proxy socket（记 P3，整机回放最后一块）。
6. **F6** 注记：`repro_bugs.sh` 加 "argv 生效" 专项断言，防软分支变盲区。
7. 修复范围承诺：均落在 `tests/`（transcript/replay/各套件头部），**`src/` 零改动**（与原始报告结论一致）。

> 后续：本归档仅登记核实与处置方案，不直接含代码修复。F1/F2/F3 建议各自/合并拆成修补 PR，合并后回到本档案补"修复 commit"列。

---

### 处置进展（F1/F2 已合并主分支；F3/F4/F6 已修并接 CI 回归）

> CI 集成：F1/F2 经 `test-tr`/`test-rp` 已在 `make test` 强制门禁内（PR #32 合并 main）。F3（`test-tcp-attack`）
> 与 F6（`test-repro`）为重型 QEMU 测试，新增独立 GitHub Actions job `regression-rr` 接入（不影响被 main 分支
> 保护绑定的 required check `test`，但每次 push/PR 红绿可见）。

| ID   | 状态     | 修复落点                 | 关键实现                                                                                                                                                                                                                                                                                                            | 验收证据                                                                                                                                                                                                      |
| ---- | -------- | ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **F1** | ✅ 已合并 | tests/replay.sh          | `replay_into` 由 `rel_ms delta sleep` 改「等上一行 ack 计数到位再注入下一行」的行级背压；ack 信号为消费路径 `[sched] wake keyboard waiter pid=.. (N bytes)` 与 `[kb] readline pid=.. -> N bytes` 的并集计数。`rel_ms` 保留仅作审计。                                                                               | 12 writefile+12 rm 多行流回放：普通时钟 **rm 12/12、无 unknown、残留 0**；`REPLAY_ICOUNT=1`（F1 原始失败条件）同样 **12/12、无 unknown、残留 0**。test-tr / test-rp / test-repro 全绿。已随 PR #32 合并。                  |
| **F2** | ✅ 已合并 | tests/transcript.sh      | `tr_send` 注入后等本行 readline 消费确认（同 ack 并集计数自增）再返回；超时 → `error: input ack timeout` **exit 2**（0/1/2 环境病语义对齐）。`tr_emit_in` 改在 ack 后记 `rel_ms`，使 in.tr 即"消费时间轴"（F4 附带）。调用方经 `TR_LOG` 注入串口日志路径（test_transcript/test_replay/repro_bugs/rp_torture 已接线）。 | 录制 golden 12 writefile+12 rm 全量落盘（rm 12/12、余 0），不复现审核侧"selftest 整条丢失"。已随 PR #32 合并。                                                                                                       |
| **F3** | ✅ 已修复 | tests/test_tcp_attack.sh | 去掉 `fuser -k`（避免共享 runner 误杀他人进程）；起服务前 `ss -ltnu` 前置检查 8080/7778/59998，命中即 **exit 2**（环境病）。结束其"端口被占 → 业务 5 连 FAIL → 误读为 TCP 被击穿"的假阳性链路。 | 端口空闲 → 正常跑完（动手实测 PASS，业务断言 closed=1/tail=TAIL/refer -1/RESULT PASS/无异常标记）；手工占用 8080 → 立即 exit 2 且不误杀。已接 CI `regression-rr`（make test-tcp-attack）。 |
| **F4** | ✅ 已加锚 | tests/transcript.sh | `tr_mark <label>` 把就绪锚固化进转录：in.tr 记 `# MARK <label> @ rel_ms (log_off=n)` 事件行（'#' 前缀 = replay_into 跳过，回放安全）+ marks 表（label\tlog_off\trel_ms）；`tr_window_after <label>` 输出锚后字节流，判据统一在就绪锚后归一化扫描，杜绝 boot 期同名输出误匹配。 | test-repro 串行实测：`mark 'ready' @ 19ms (log_off=15478)`，BUG-B argv 判据经窗口化后仍 `[ok] argv 生效`；in.tr 由 13→14（锚事件行），EXIT 0。 |
| **F5** | ⏳ 未处理（P3） | — | net.in.tr 姊妹流 + 回放器按 rel_ms 重放到 peer/proxy socket（整机回放最后一块）。 | — |
| **F6** | ✅ 已加固 | tests/repro_bugs.sh | BUG-B 从软分支改硬断言：按 `[ls]  *out2\.elf` 目录条目判定 argv 生效（旧 grep 会误匹配命令回显且永不红）；argv 丢失即 `FAIL++`。判据经 tr_window_after ready 归一化。 | test-repro 串行全绿，BUG-A/BUG-B 均未复现 → `[ok] argv 生效：/out2.elf 已在 ls 中列出`。已接 CI `regression-rr`（make test-repro）。 |

> 说明：审核报告原述"ack=`[kb] readline pid=`"，实测（源码 + 串口日志）确认常态注入路径（读方已阻塞）的消费信号实为 `[sched] wake keyboard waiter pid=.. (N bytes)`，`[kb] readline -> N bytes` 仅在行缓冲已就绪路径出现——修复按**两者并集计数**实现，两条路径均覆盖。