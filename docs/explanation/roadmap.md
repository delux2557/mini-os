# 演进路线（Roadmap）

> 目标：以"最小可用、逐步增量"的方式，把一台裸机从零变成一个
> 具备多任务、文件系统、网络、可执行程序加载与**自举能力**的微型操作系统。
> 项目已越过"功能积累期"（教学闭环达成），进入 **"收尾-加固-沉淀"** 阶段——
> 最终交付物 = 可运行内核（五层回归全绿）+ 工程方法论文档 + "AI 能写操作系统"的实证案例。

## 已完成里程碑（已归档）

> v0.1 ~ v0.33 里程碑速览已归档至 [history/roadmap-milestones.md](../history/roadmap-milestones.md)（只读）。
> 版本历史唯一事实源是 [changelog.md](../reference/changelog.md)；本处不再维护重复表格。

## 下一步规划

> **当前路线以下方三阶段为准**；"支线 A/B/C"为历史规划存档（多数条目已勾选完成，
> 保留作演进记录）。

### 项目阶段判断

| 维度     | 状态                      | 判断        |
| ------ | ----------------------- | --------- |
| 核心概念覆盖 | 进程/内存/文件/网络/工具链全部完成     | 教学闭环已达成   |
| 工程质量   | 五层测试、零告警、纯逻辑可单测、四件套文档   | 工程成熟度高    |
| 代码规模   | \~500KB / 28 个版本        | 接近维护临界点   |
| 自举能力   | guest 内写-编-跑闭环、编译器不动点验证 | 已具备自我演化能力 |

**结论**：继续无限堆砌新子系统边际收益递减，而跨子系统组合的维护成本递增
（BUG-020/025/026 已展示该趋势）。下一步按三阶段推进，不再追逐版本号。

### 阶段一「收尾」（只补欠账，不开新坑）

* ✅ **P0 泄漏修复**（v0.28，BUG-027/028）：`sys_map_page` 记账槽满分配前拒绝；
  exec 路径归还 `load_frames` 数组

* ✅ **DHCP 租期续约**（v0.28，RFC 2131 §4.4.5）：T1 单播 RENEW / T2 广播 REBIND /
  ACK 重置 / NAK·超时重新获取

* ✅ **sys\_map\_page 容量上限显式化**：超 8 页返回 -1 而非静默泄漏（BUG-027 的一部分）

* ✅ **BUG-031 文件槽泄漏**（v0.30，用户实操报告 + 复现）：全局 `fs_files[8]`
  无进程归属、退出路径不清理——一次编译失败（cc500 parse error）即烧掉 slot2、
  污染整条工具链直到重启。修复：槽记打开者 pid，进程退出按归属归还
  （`fs_files_close_pid`）；per-process fd 表（打开文件表入 PCB）仍留作架构债

* ✅ **BUG-032 cc500 入口桩丢 argv**（v0.30，用户实操报告 + 复现）：`be_start` 裸
  `call` 不编组 argc/argv，自编译产物静默吞 argv。修复：入口桩 call 前压 argv/argc
  （与 cc500 "首参 8(%esp)/末参 4(%esp)" 约定对齐）；v0.27b "命令行路径"现对自编译
  产物也成立

* ✅ **`fork_frames`** **动态化**（v0.30，BUG-035，=OBS-002）：`sched_fork` 先数需深拷贝
  页数再按需 kmalloc 动态数组（同 `own_frames`），退出 kfree——大进程（bigdemo 28 页）
  fork 不再受 24 帧硬编码上限限制

* [可选] **pipe（管道）**：经典 IPC 补全（字节流 vs 消息队列的离散消息，互为补充）。
  ⚠️ 定性为**新功能**而非欠账，与"收尾"原则有张力——若做，归入"如果还想做深"选项

### 阶段二「加固」（不增功能，增信心）

* ✅ **fuzz**（v0.29，`tests/fuzz_parse.c`）：确定性 xorshift32 注入随机路径/随机字节，
  覆盖 `fs_walk` / `elf_load_range` / `net_eth_type` / `arp` / `ip_parse` / `udp_parse` /
  `icmp_parse` / `dhcp_parse_reply`；ASan+UBSan 宿主侧跑（60k 轮缺省、FUZZ\_ITERS 可调），
  发现并修复 **BUG-029**（`icmp_parse` len<14 时 `len-14` 下溢 + `frame+14` 越界读），
  已集成 `run_host_tests.sh` 强制回归

* ✅ **内核堆审计**（v0.29，`heap_audit` 挂入 `kern_audit`）：遍历 `block_t` 链表校验
  magic/free 一致性、size 上界，防 next 指针成环/悬垂；`used/free` 记账计数器与遍历
  统计对账（泄漏/双重释放/写越界破坏块头都会漂移）；报告碎片；宿主单测 + QEMU selftest
  双重锁定（`[audit] heap ok`）

* **record/replay 地基（工程进度：P1 ✅ P2 ✅ P3 ✅【闭环完成】；dev 侧基建，对接"AI agent 演练场/测评"）**
  **技术前提（先对齐再动工）**：QEMU `-icount` 只保证**内核执行**确定（虚拟时间=指令计数，
  定时器/中断/调度同输入同输出），**不约束整场测试**——三门外部输入（串口 FIFO 注入 / 网络
  SLIRP·宿主转发器 / host 侧 kill·sleep 时序）不受 icount 约束。故完整形态 = 两层：**icount
  确定性内核 + 输入流时间戳化录放**。公共时钟须用 **QEMU icount 虚拟时钟**（host 侧经 QMP/监控
  读回），**非 guest 裸** **`sys_getticks`**——repro\_bugs.sh 只是"脚本化"未录时间关系，即缺此层。
  分三阶段、每步独立可验收、保持 9 层 CI 绿（P1-P3 均为 tests/+Makefile+脚本的 dev 侧变更）：

  * **✅ P1 确定性启动（v1.4 落地，`tests/test_determinism.sh`** **+** **`make test-det`）**：两次 QEMU
    `-icount shift=auto,align=on,sleep=on` 冷启动，串口日志**逐字节 diff** 判定确定性。实测：
    icount 下启动段（含 **DHCP OFFER/ACK 网络握手**）两次运行**逐字节一致** = 内核同输入同输出的
    铁证（P1 验收本质）。**诚实发现**：交互回归脚本（qemu\_regression 的 HMP sendkey / serial /
    persist / cc500）走 host 墙钟轮询（`wait_for`/`sleep`），与 icount 虚拟时钟流速**不匹配**，
    icount 下 run 窗内超时误报——这是**交互脚本的 host 墙钟依赖被暴露**（恰为 P1 意义），故
    **不回编**这些脚本；icount 确定性验证独立收编为 `test-det`。网络层**不承诺** icount（SLIRP
    依赖 host 时间，见下"边界"降级），`test-det` 用纯冷启动含 DHCP 握手证明在**无注入输入流**
    下确定性已成立；未来交互确定性交给 P2 transcript 录放。

  * **✅ P2 transcript 固化（v1.4.1 落地，`tests/transcript.sh`** **+** **`tests/test_transcript.sh`** **+
    `make test-tr`）**：录制内核 `tr_start/tr_send/tr_snapshot/tr_abort/tr_finish` 把输入命令流
    （`*.in.tr`，列=序号/相对ms/命令，可重放审计）与输出字节流（`*.out.tr`）固化到
    `build/transcripts/<runid>/`。验收三连：① 成功固化（in/out/RESULT=PASS 产物完整）；
    ② **失败自动归档**——`tr_abort` 在失败点名固化现场并标 `RESULT=FAIL`（"人为触发失败可得可复现
    transcript"达成）；③ 复现性雏形——两次冷启同命令集、里程碑语义行逐字节一致。
    **诚实发现**：非 icount 两次运行 `Hello ticks=296/297` 差 1——guest tick 值随墙钟调度浮动，
    非语义差异（**印证"公共时钟须用 icount 虚拟时钟、非 guest tick"**）；故复现性比对按 `ticks=N`
    pin 掉噪音，真逐字节确定性交给 P1 test-det。**相对 ms 用 host 墙钟（起记时刻打点）；icount
    虚拟时钟锚点与 P3 严格回放差分（含时间关系）留待 P3**。

  * **✅ P3 replay 验证（v1.4.2 落地，`tests/replay.sh`** **+** **`tests/test_replay.sh`** **+** **`make test-rp`）**：
    回放器 `replay_into` 消费 `*.in.tr`（按 seq/rel\_ms/payload 打拍注入串口 + 等完成信号）驱动
    真实内核路径。验收闭环：从 bugs.md 抽 **BUG-026**（cc500 形参列表 EOF 未闭合→死循环），录含
    其触发输入（`writefile` 写 `int main(int x` + `ccrun`）的 transcript → 回放 → 修复版见
    `cc500: error at`（exit(1) 不死循环）——证明回放抓住 bug 表现。
    **诚实发现（P3 开发实测，均为已知边界，已通过调整规避）**：① icount(TCG 逐条虚拟化) 下 cc500
    编译器慢到分钟级 + 后台 demo 应用（`[B]` tick / net recvfrom）持续打印抢 tick → 回放**不用
    icount**，bug 闭环靠**信号断言**（`cc500: error at`）而非逐字节 diff（逐字节确定性已由
    P1/test-det 承担）；② 后台 demo 日志永不静止 → 回放 end 判据用**完成信号**而非"日志静止"；
    ③ 跨**独立**冷启动的里程碑一致不机械稳定（trace-heavy 交织点抖动）→ 两遍一致性作可选
    `REPLAY_VERIFY=1` soft 检查，硬门禁是单遍 bug 闭环。完整照 roadmap 原文边界依旧成立。
    **边界（诚实）**：`-icount` × SLIRP/外部进程时序是 QEMU 文档明示的交互点 → 网络层若红则降级为
    "icount 只用于无网络交互层 + 网络层走 P2 transcript 录放"，地基仍成立。**gdb reverse-debugging**
    仅作失败后人工单步逆向定位（开销大），不进回归主路径（P1-P3 不依赖它）。

  * **✅ record/replay 工程收尾（v1.4.4，`tests/tr2sqlite.py`** **+ 无网络路径** **`-nic none`）**：P3 闭环后
    两件零侵入加固。**① 网卡与 icount 慢的边界澄清 + 处置**：实测 `[B]` tick / net recvfrom 是用户态
    demo（procB / sockdemo）抢指令预算，**非网卡导致**；但启动期默认 e1000 + DHCP 握手确实给 icount
    增加墙钟开销。故对不需要网络的回放/编译/录制三条路径统一 `-nic none`——内核无网卡时优雅跳过
    （`e1000 not found on PCI` + `selftest skipped (no e1000)`，不挂起），去掉启动期等待又少一个
    非确定源；网络回归保持挂 e1000 不动。**② sqlite 分析索引**：新增 `tr2sqlite.py` 把 `.tr` 增量
    导入 sqlite（`transcripts` / `in_events` / `out_rows` 三表，幂等、只读旁路）。**设计前提**：
    录放主路径仍 `.tr` 文本"证据原件"（确定性/可 diff/可归档），sqlite 只作"放大镜"，坏了绝不影响
    录放正确性；归档量大起来才有跨 runid 聚合/`LIKE` 检索的爽感，是"想试随时能试"的纯增量工具。

  * **✅ record/replay 接 repro\_bugs.sh（v1.4.5，`make test-repro`）**：首方复现脚本接入录放——
    BUG-A/BUG-B 复现命令流改经 `tr_send` 录制为 `.in.tr/.out.tr`（含 wait 驱动的**真实相对 ms**，
    补上"repro\_bugs.sh 只脚本化、未录时间关系"缺口）；修复版"未复现"即 RESULT=PASS 证据，若回归
    （`compile FAIL`/`output setup fail`）当场标 FAIL。录制 transcript 实测可被 `replay.sh` 消费
    重放复现固定行为（`[ccboot] byte-identical PASS` / `out2.elf` 构建 / `bad.c`→`cc500: error at`），
    无网络路径同加 `-nic none`。

  * ✅ **独立测评补格（fix 分支，2026-09-03，旁证）**：独立推演确认"日志撕裂"即为 RR 判据失真的
    **总根因 K1**，并根治为三处修复（详情见 [docs/bugs.md](../reference/bugs.md) BUG-051~053）：
    **BUG-051** `serial_printf/puts` 整行 IRQ 原子化（`pushfl/cli…popfl` 状态保持，零污染 icount 路径）
    → 串口行不再在字符粒度被抢占撕裂；**BUG-052** 收掉 test-tr 复现性步 ~67% 假红（`pick()` 里程碑
    子串化 + 快照锚点有界等末条完成里程碑，连跑 5 次 0 假红）——印证 P2 验收③"里程碑行稳定"依赖的
    正是串口行原子性；**BUG-053** `BUILD ?=` 可覆盖，为并发重负荷 harness 隔离提供使能原语。
    伴生收益：K1 造成的 **ack 计数握手失步**（`edge_atk.log` 回显被切开）随之消失，RR/AI-agent
    输入同步协议恢复确定。真逐字节确定性仍归 P1 test-det，未越权（详见 docs/history/external-reviews/mini-os-eval-log_9711cbc.md）。

* **record/replay「尺子」元工程演进（自检与防假阴性，系统工程视角的最佳实践沉淀）**
  > 定位再校准：record/replay 给的不是**正确性证明**，是**可复现基线 + 差分告警能力**——
  > 它把"怀疑 bug"升级为"**可举证的怀疑**"，但不能单独宣告"无 bug"。既然它是度量工具（尺子），
  > 就须按度量系统的纪律对待：**量程、标定、自检、可置疑**。以下是对尺子本身的演进建议（相对
  > BUG-051~053 落地后的加固线，非阻塞、纯增量）。

  * **模型分层：先辨"尺子"再辨"缺陷"**。任何一次"尺子亮灯"，处置顺序是**先排除第①层（尺子
    不准）→ 才进入第②层（系统真有 bug）**。P1/P2/P3 三段（icount 确定性固化 → `.in.tr/out.tr`
    固化 → replay 差分）任一环节晃，都会污染结论；K1/撕裂（BUG-051 前 ~67% 假红）即第①层未稳
    的活例。故**每次修系统前，先证明尺子稳**——这条纪律要写进复现工作流，而不是当场判别。

  * **外部真理锚定：从"相对 oracle"跨到"对/错判据"**。尺子（record/replay 差分）是**相对 oracle**，
    只能证 `A≈B`（这次=上次），永远到不了 `A∈正确`；Rice 定理决定了"无 bug 证明"一般不可判定——
    故行业的正解不是"证明可靠"，而是**用不同强度的"外部真理"判据锚定 + 三角互证 + 管理残余风险**。
    把这些判据从弱到强列出，作为补锚的 checklist（按本仓库现状标注）：
    * **症状判据**（禁已知坏：退出码/不挂起/不崩溃）——repo 已有（`repro_bugs` `exit(1)`、
      `[selftest] PASS`）。**不需要 golden**，对着"坏"说判，天然是外部锚。
    * **性质/不变量**（spec 派生的谓词：无泄漏/无越界/无 double-free）——repo 已有
      （`[audit] heap ok`、guard-page→fault）。
    * **参考实现/委外共识**（第二个独立实现或已知对的参考去 diff）——repo **最强锚已在不自知地
      用**：cc500 自举**逐字节不动点**（`P1==P2` 时隔两层互相复现，自洽性钢尺）；可再补 `cc vs gcc`
      做边界程序集的第二把尺。
    * **形式/模型验证**（Coq 证明、TLA+ 状态机、model checker）——教学成本高，仅限核心 TCB，暂无投入。
    * **golden 签证**（golden 一旦被人工/异实现复核过即"签证"为 blessed、冻结当外部锚；其余自动录制
      一律标"自举、弱锚"）。把 `.out.tr` 的自举来源标注可信度，就从"弱自比"变"外部锚"。

  * **防假阴性四原则（对尺子的加固，按回报排序）**：
    * **① 输入覆盖 = 量程底线**：尺子量得到的前提是命令流触达。未被 `.in.tr` 覆盖的路径（错误
      分支/边界 syscall）是"根本照不到"的真问题。对策：把差分告警从"不匹配才响"升级为
      **出现未见过的新里程碑/新行也告警**（新行为未必错，但必须可解释，尤其偶发 tick）。
    * **② golden 自举 = 标定缺陷**：`.out.tr` 的"黄金"来自首次录制——若那次带错，回放永远
      "一致地错"。对策：**golden 签证 + 外部标定**（见上"外部真理锚定"）——人工/异实现复核过的
      golden 标 `blessed`、冻结当外部锚；自动录制标"自举弱锚"；并对复核过边角用独立 checker 重算
      契约指纹，不把"首次录制"当作先验正确。
    * **③ 掩蔽/锚点是量程缩窄**：差分掩掉 tick 行、判据只锚 `[selftest] PASS` 里程碑 + 有界等待，
      → 里程碑之后、被掩的行在量程外。对策：**显式登记掩蔽清单**（豁免哪些行、为何），里程碑外叠
      一个"区间冒烟"判据，别让"掩 tick"退化成无人记得的临时 hack。
    * **④ 确定性边界 = 量程声明**：`-icount` 只在同款 QEMU + 给定 config 成立；真机/慢后端/他版
      QEMU 时序不同 → 尺子无刻度。对策：**在判据上声明量程**（"本判据仅在 X 环境成立"），越界
      域降级为"非确定性冒烟"，**不宣称无 bug**。真逐字节确定性仍归 P1 test-det。

  * **尺子自检（承接 OBS-RR-1/2/3，队伍待排期）**：翻这三条即对尺子自身的加固——**OBS-RR-1**
    ack 信号依赖 `debug` 关键词，建议独立通道防误判；**OBS-RR-2** 回放 ack 未齐仅告警，建议
    `RP_ACK_STRICT` 强一致；**OBS-RR-3** `.in.tr/.out.tr` header 无格式版本，建议加版本号防
    工具演进互不识别。三者均为"尺子没自检"的实例，属低成本高回报的尺子加固。
    对应 bugs.md：externals 深审 OBS-*；量程声明若要入 CI 判据，落为独立加固 PR。

  * **交付动作（最小集，暂不立项，节能）**：① 在 README/复现文档里写死"先辨尺子再辨缺陷"的
    复现工作流；② 差分告警"新行为即疑点"可在 `rp_torture.sh` 加一行可观测开关（观测先行）；
    ③ 掩蔽清单以注释形式就地登记到 `rp_torture.sh` / 判据脚本。
    **外部锚固化（承接上"外部真理锚定"，随需要立项）**：④ 把已有 audit 性质判据（`[audit] heap ok`、
    guard-page→fault）**声明为"独立于 golden 的性质锚"**并纳入门禁说明，作为对系统可靠性的正式证据；
    ⑤ golden 签证：给 `blessed` golden 加来源标注 + 冻结，其余标"自举弱锚"；⑥ 可选第二把尺：
    对 cc500 边界程序集加 `cc vs gcc` 差分进 CI。以上均纯增量、不动录放主路径、不碰真 TCP/SMP/HAL
    ——与本仓库"纯增量、可复现测试"红线一致。

* ✅ **回归盲区补格**（v0.29）：

  * `deep`/已生长栈 × fork/exec 组合：新增 `deepfork` / `deepexec` 演示并挂入
    qemu + serial 回归；**顺带抓到并修复 BUG-030**（fork 子进程栈在父槽、守卫按子 pid
    推导槽位误判缺页——改由实际栈位置 `stack_bottom` 推导）

  * brk 收缩-再涨路径：heapdemo step 4（收缩回 8KB→sbrk 再涨复用已映射页）已覆盖

  * 编译产物 × 持久化：test\_persist.sh S10（writefile→ccrun→save→重启→run）已覆盖

### 能力边界：宿主代理——把 https/ssh「接进」demo（讨论定论）

> 背景：guest 只有极简虚拟 TCP、落 http 明文，**原生实现 https（TLS 证书链、握手/suite、E2E 加密）成本过高**。
> 但宿主转发器（UDP proxy → host 侧 HTTP）已是"guest 发请求 → 宿主转发"的出站网关基座——引申方向即：让宿主替 guest 做外网协议。

* **结论（可行 = 标准「TLS 终止型正向代理 / egress 网关」）**：guest 发 http 明文 → 宿主 Python（`requests` 发起真实 https + `verify=True` 证书校验）→ 明文响应回传 guest。guest 全程 http，不碰一字节 TLS。**SSH 同理**（宿主起 ssh 客户端/网关替 guest 建连）——本质是"宿主把任意 TCP 隧道化"。
* **必须清醒的定性（别把代理当 guest 支持 https）**：
  1. 这不是 mini-os 支持 https/ssh，而是**宿主替它做密码学**——信任锚在宿主，guest 无密钥、不验证书、看全明文，端到端保密/防篡改为零；对 demo/CI/教学足够，对"安全HTTPS"则不成立；
  2. https 需由 guest 携带 host/SNI 让 forwarder 定目标，并维护「guest 请求 ↔ 真实 TLS 连接」的薄映射层；
  3. SSH 走此路只剩"连接能力"——其核心卖点（端到端加密+认证）在宿主终止后**尽失**，勿标榜安全。
* **对录放/工程的意义（≈ F5 原料）**：真正值得做的是 guest/forwarder 间一层**"出站连接+字节流的可描述抽象"**（guest 声明：连谁、发什么、等什么），forwarder 翻译成真实 https/ssh——这正是交接单 **F5（`net.in.tr` 姊妹流 + 回放到 peer/proxy socket）**：能录制网络流，就能把代理退化为可复现回归。
* **建议顺序**：先落一个最小 demo（guest 发 http → forwarder 拉真实 https 站点 → 明文回传验证链路），再回头做 F5 的网络流录制。
* **分类**：宿主侧/dev 侧能力，非内核功能；不归阶段一/二欠账，作"想继续谈网络深度"时的可选自制线。

### 阶段三「沉淀」（不再是版本号）

> 从"持续开发的仓库"变为"可交付的教学产品"——项目的最终价值不在代码行数，
> 而在"能被多少人学会"。

1. **教学文档系列**：每子系统一篇"从零到一"（引导与保护模式 → 中断与系统调用 →
   分页与隔离 → 调度 → 文件系统 → 网络 → 自举），附最小可运行代码片段 + 思考题
2. **交互式实验手册**：利用已有写-编-跑闭环，读者在 guest 内用 `writefile` + `ccrun`
   编写小程序，亲手体验 `fork`/`brk`/`socket`/`ccboot`——比"读代码"有效得多
3. **开源发布**：定位 **"AI 辅助系统编程的完整案例研究"**——BUG 库的根因/修复/回归
   记录本身就是极有价值的工程方法论素材；演示录屏（`make run` → `selftest PASS`、
   `ccboot` 自举仪式）

### agent 演练场（新主线：把项目浇铸成"agent 友好的平台"）

> 状态：**阶段0已落地**（task 契约 + 可复用 gate 判据）。方向：把"agent 改内核"从
> 无结构的自由操作，升级为"任务契约 + 客观判分 + 可复用判定"的受控闭环。
> 战略依据：record/replay + 契约指纹 + 基线巡检 + 内核自审计，本就是为"AI 能否可靠地
> 改动一个大系统"设计的能力；演练场只是把这些资产对 agent **开放化、契约化**。
> 与红线兼容：纯增量、不动 guest 内录放主路径、不碰真 TCP/SMP/HAL，是组合回报最高的方向。

**已有垫脚石（agent 已能在里边干活）**：

| 能力      | 落点                                           | 意义                             |
| ------- | -------------------------------------------- | ------------------------------ |
| 写-编-跑闭环 | `cc500` + `writefile`/`ccrun`/`ccboot`       | agent 能在 guest 内写·编·跑任意用户程序并自举 |
| 可复现测试   | record/replay + transcript + `repro_bugs.sh` | agent 改动后行为可被客观判定是否漂移          |
| 客观判据    | `SYS_KERN_AUDIT`、`selftest`、基线巡检/契约指纹        | 内核"自己报健康"，改动好坏可量化              |
| 安全前提    | copyin/copyout + 地址空间隔离 + 进程级 fd 表           | 敢于让 agent 运行任意编译产物的隔离基础        |

**落地路径（从轻到重，每步可独立验收、CI 全绿才进下一步）**

* **✅ 阶段0：task 契约 + 可复用 gate 判据（v-tbd；`tests/arena/`）**。定义机器可读的
  `task.json`（`id`/`title`/`prompt`/`gates`/`tolerance`）把"任务→判分"落成规范；
  把 baseline\_check.py 的判定逻辑（契约指纹漂移、输出行数/字节漂移、契约内容丢失）
  抽成**无副作用纯函数 gate**（`tests/arena/gate.py`），供基线巡检与任务判分**单一来源复用**；
  `run.py` 从 transcript 目录构造判定数据，`task.py` 裁判器据此出 `PASS/FAIL` + 具体证据。
  验收：同一契约类漂移（如 torture-a）返回一致 PASS；跨契约/构造漂移边界返回 FAIL(exit 1)
  并列出丢失的契约行；`baseline_check.py` 复用后行为与原实现一致。

* **✅ 阶段1：agent 网关（`tests/arena/qw.py`）**。统一命令**输出 JSON** envelope
  `{"ok":bool,"cmd":…,"data":{…}}`，任何诊断走 stderr、stdout 只有 JSON，LLM 无需解析人读字符串。
  - **`status <db>`**：DB 概览（run 分组/最近 run/契约哈希/stage 行数）
  - **`rebuild <db> [--kind]`**：契约指纹+输出量基线判定，含 `alarm_count` + 每 run 的 gate
    结果与阶段耗时趋势（stages P50/P95/latest）
  - **`submit <task.json> --run <dir> [--base <dir>]`**：对一轮 transcript 跑 task 判定 →
    `{verdict:PASS/FAIL, gates:[{name,ok,msg}]}`；漂移时 exit 1 并在 msg 里列丢失契约行
  - **`task [list|<json>]`**：列出/读取任务契约；**`gates`**：列出已注册判据
  判据全部复用 `gate.py`/`run.py`/`task.py`（单一来源），本文件只做"命令→JSON"胶水；
  不重开 sqlite 写回、不动录放主路径。验收：各命令对既有 torture 数据输出正确 JSON，
  同契约 PASS(exit0)、漂移 FAIL(exit1) 且给出契约行证据。

* **✅ 阶段2：评测器（判分闭环，`tests/arena/evaluate.py` + `qw.py eval`）**。串起
  replay 差分 + 基线巡检 + 内核标记审计，对一轮 transcript 一次得出 `PASS/WARN/FAIL`，
  并落到"**在哪一步 + .tr 证据**"。四步判据：
  - **result**：读转录 RESULT 的 `# result:`——挂了/崩了 = 前提 FAIL（缺文件 → WARN）
  - **baseline**：复用 `gate.run_gates`（contract_hash/out_lines/out_bytes/契约内容）对照基线 run，
    判据集取自 task.gates（单一来源）
  - **audit**：扫 out.tr 的内核致命/越权/溢出标记（FATAL/double free/PAGE FAULT/STACK OVERFLOW/
    panic/BUG）+ 内核自审计失败行；已知预期隔离演示(procCrash)`crash demo:…` 上下文排除，命中给行号证据
  - **replay**：（可选 `--replay-log`）重放契约行集合 vs 当轮契约行集合——现场能否复原；
    缺证据 → WARN
  汇总：任一 FAIL → FAIL；否则有 WARN → WARN；全 PASS → PASS（`qw.py eval` exit：0/1/2 对应
  PASS/WARN/FAIL）。验收：对既有 torture transcript 出正确 JSON：全 PASS(exit0)、契约/审计漂移
  FAIL(exit1) 且 `steps[].evidence` 给出丢失契约行或 FATAL 行号、缺 replay 证据 WARN。

* **阶段3：运行编排（可选外壳，最后做，MVP 可不上）**：独立 QEMU 实例、超时 kill、
  失败快照捡出、并发排队；做到这里才谈得上"开放平台"。

**诚实边界**：guest 能跑任意代码，是**教学沙箱，不做公网多租户开放**；真要公开先加资源
配额与隔离。MVP 只做阶段0-2（契约 + JSON 网关 + 评测器），不先铺 QEMU 沙箱/并发即外壳。

### 网络抽象层与虚拟 TCP（netif + 间接 TCP）——已完成主线

> **状态：主线已全部落地**（v1.1 四步 → v1.2 可靠收发 → v1.3 上行滑动窗口；changelog v1.1~v1.3）。
> 完整决策史（D1-D6 + Step 1-4 + 薄→厚演进预留）已归档：[history/netif-roadmap-v1.1.md](../history/netif-roadmap-v1.1.md)（只读）。
> 协议契约以 `docs/tcp-session-proto.md` / `tcp-thin-api.md` / `tcp-mtu-fail.md` 为准（动码前定稿）。

- **✅ 上行滑动窗口（v1.3）**：停-等 → N 在途（guest 发送窗口 `TCP_TXWIN=8` + 累计 ACK + 超时重传），吞吐 1/RTT → W/RTT。
- **候选（未做）——下行滑动窗口**：host→guest 下行仍停-等（转发器 ≤1 报在途）。提速需转发器发送侧窗口 + guest 接收窗口/累计 ACK（现只回单一期望 seq）。上限由 SLIP 慢通道 L2 与两端缓冲决定，初期取保守小值；与上行正交、技术镜像，可独立推进，不破坏薄包装 API、会话表与协议头结构。
- **残余待收口**：e1000 DHCP BOOTP 组帧（`e1000_dhcp_tick` 直调）推迟到 HAL 阶段（红线：无真实 ARM 硬件前不做 HAL）。
### 红线（明确不做）

| 方向                      | 理由                                                                                                                         |
| ----------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| guest 内 TCP 状态机         | 复杂度过高、测试面超纲。不实现 guest 内真 TCP；虚拟 TCP 用薄包装（真 TCP 状态机只在宿主转发器，guest 演进上限 = 厚包装伪状态机），见上"网络抽象层"节。若确需真 TCP 属 virtio-net 另起炉灶的独立项目 |
| 多核 / SMP                | 重写调度/锁/页表模型，等于重写                                                                                                           |
| HAL / ARM 移植            | 无真实目标硬件，抽象层设计必然过度；等真有 ARM 板子                                                                                               |
| 动态链接 / ELF 重定位          | 已标注"黑洞，吃掉项目余生"                                                                                                             |
| 移植 GCC/clang/TinyCC 完整体 | 同上                                                                                                                         |
| 图形子系统                   | 偏离核心教学定位                                                                                                                   |

### 支线 A：继续做深 x86 内核（v0.16+）

* ~~用户栈守卫页（guard page）与栈溢出检测~~ ✅ v0.13 已完成

* ~~更完整的文件系统（目录层级、文件偏移定位/追加写、间接块）~~ ✅ v0.14 已完成

* ~~补全 fork/exec 的 wait 语义（wait/waitpid、孤儿清理）~~ ✅ v0.15 已完成

* ~~用户态 CRT 收口（app\_main 返回即 exit）+ ATA 真盘持久化 + 单行自检~~ ✅ v0.16 已完成

* ~~syscall 边界校验（copyin/copyout）~~ ✅ v0.17 已完成

* ~~网络：e1000 驱动 + 极简协议栈（ARP，QEMU/SLIRP 端到端）~~ ✅ v0.18 已完成

* ~~网络加厚：极简 IP/UDP（纯逻辑可宿主单测）~~ ✅ v0.19 已完成

* ~~用户态 UDP socket（sys\_net\_\* + sockdemo 端到端回环）~~ ✅ v0.20 已完成

* ~~内核自审计（不变量检查）+ syscall 边界契约化~~ ✅ v0.21 已完成

* ~~socket 演示可交互化（shell~~ ~~`netping`~~ ~~命令）~~ ✅ v0.22 已完成

* ~~ICMP 回显（PING 通宿主）~~ ✅ v0.23 已完成

* ~~UDP 校验和错误路径~~ ✅ v0.24 已完成

* ~~DHCP 静态 IP 可配置化（动态获取，失败回退单一静态配置点）~~ ✅ v0.25 已完成

* 候选下一步（按价值排序）：

  * **网络进一步可用化**：~~DHCP 租期续约（T1/T2 定时 renew）~~ ✅ **v0.28 已完成**；
    TCP 状态机暂缓（复杂度高，非"最小可演进"核心）

  * 真实硬件引导（GRUB/ISO）——串口终端（v0.10）已就绪，届时可直接在真机串口上交互调试；
    注意真机网卡/磁盘与模拟器不同（非 82540EM / 多为 AHCI），网络与持久化验证以模拟器为准

  * 可选：多级间接块/索引节点、mmap/写时复制(COW) fork、信号与信号处理、进程槽扩容

### 支线 B：为移植 ARM 预留架构（HAL 抽象层）

* 抽出 **HAL**（硬件抽象层）：把 GDT/IDT/PIC/PIT/串口/键盘等 x86 特有操作封装成
  `hal_*` 接口，内核其余部分只依赖 HAL

* 地址空间抽象：把"页表/线性地址"抽象为 `vm_space`，隔离 x86 分页细节

* 上下文切换抽象：把 `isr.s` 的寄存器现场/切换路径抽象为架构相关汇编接口

* 目标：换 CPU 时只重写 HAL + 少量汇编 + 链接脚本，调度/内存/文件系统/IPC 全复用

* 风险提示：这是较大重构，**届时必须在独立分支上做**（如 `git switch -c feature/hal`），
  HAL 落地 + 回归全绿后再合并回主线，避免破坏 x86 主线可运行状态

> 建议顺序：优先支线 C 把"agent 在 guest 内写-编-跑闭环"立起来——
> 先做 v0.26 容量三连（栈按需生长 / sys\_brk 用户堆 / ELF 加载去上限，全纯扩展、可宿主单测），
> 再按 27a/27b → 28 → 29 拆小推进工具链与自举（汇编器+链接器 → C 前端 → libc/crt0 → 自举仪式）。
> 此间 v0.23-v0.25 网络加厚（ICMP / UDP 校验和 / DHCP 动态取 IP）已先后完成，
> 网络已从"驱动"做到"可交互验证 + 经典 ping + 坏包防线 + 动态地址配置"。
> P2 并行可插：DHCP 租期续约、record/replay（QEMU -icount reverse-debug，勿引 rr）。
> 真机 GRUB/ISO 冒烟在工具链落地后再做（以模拟器验证驱动为准）。
> 等 x86 特性攒够、且真有 ARM 目标时再启动 HAL 重构——因为独立地址空间已牵动页表/切换，
> 届时抽象 HAL 收益最大、返工最少。HAL 属破坏性重构，需开分支。

### 支线 C（采纳评估简报·新主线）：agent 在 guest 内完成「写-编-跑」闭环

> **战略依据**：v0.17 copyin/copyout 的真正意义 = "敢让 agent 运行任意编译产物"的安全前提
> （已在）；运行任意程序后，内核 `SYS_KERN_AUDIT`（v0.21）即裁判。把"agent 维护的内核"
> 升级为"agent 能在里面干活的世界"，是测评体系的终极任务形态、教学链终点章、真机叙事收官。
> 现状盘点（均已对代码核实）：写文件✅ FS syscall 齐备但 shell 缺 `writefile`/重定向；
> 编译❌ guest 内无编译器；运行✅ 但被三处容量卡死（见 v0.26）。

* **v0.26「容量三连」（纯扩展，每项都是已验证机制的组合，风险低）**

  * **用户栈按需生长**：~~现每进程 8KB 槽固定（守卫 4K + 栈 4K）。做法：多页槽 + 守卫页随栈
    下移；`stack_guard_hit`（guard.c，现二态 0/1）扩为三态 OK/GROWTH/BOOM，pf\_handler 命中
    "栈区且距当前栈页 1 页以内" → 补映射、守卫页下移；其余维持原判定。
    \= v0.3 懒分配 + v0.13 守卫页两个已验证机制的组合。~~ ✅ **v0.26#1 已完成**
    （32KB 槽 = 硬底守卫页 + 28KB 可生长栈区，三态判定，`deep` 演示 12KB 递归触发 3 次生长）

  * **`sys_brk`** **用户堆**：~~现无堆（`map_frames[8]`~~ ~~固定、`sys_map_page`~~ ~~一次一页）。在私有页
    之上开可伸缩区，记账进 PCB；记账表 kmalloc 动态化（为编译器 malloc 铺路）。~~ ✅ **v0.26#2 已完成**
    （SYS\_BRK 查询/设置 program break，堆区 320KB，扩展按页补映射、收缩保留映射复用，
    `heapdemo` 演示 + `test_brk` 宿主单测）

  * **ELF 加载去上限**：~~`load_frames[APP_MAXFRAMES=8]`~~ ~~改动态列表、app 区 16KB 扩 MB 级；
    顺带解决~~ ~~`APP_LINK`~~ ~~单槽掩护的结构债（并发跑两个同链接地址程序）。~~ ✅ **v0.26#3 已完成**
    （load\_frames/own\_frames 动态化、app 区 1MB、用户空间 16MB；`bigdemo` 70KB/21 帧验证）

  * 三项均可纯逻辑化宿主单测（stack\_guard\_hit 边界、brk 状态机、加载器 frames 记账），
    延续现有测试风格。

* **v0.27-29「工具链与自举」（迄今最大单版本，必须拆小）**

  * **移植对象修正**：简报原指"Rob Pike c5"——核实后 c5 大概率是 8086 16 位版本，与 i386
    32 位平坦模型不匹配，**不直接采用**。更稳妥候选：

    * **cc500**（E. Grimley-Evans，~750 行）：stdin 读 C → stdout 出 **x86-32 ELF**，自托管、
      无 libc 依赖（内置 exit/getchar/malloc/putchar 机器码，malloc 用 brk 实现）——
      与我们的 ELF 加载器 + v0.26 `sys_brk` 天然衔接（GPL-2.0，参考/自写）；

    * 或自写 C 子集前端 + 简单 x86-32 代码生成。

    * 可行性锚点（简报成立）：PWB/C 曾以 56KB 内存在 PDP-11 自举；128MB QEMU + 容量三连后
      属"过于富裕"的尺度。

  * **~~Micro-OS libc~~**：~~user\_lib.h 已是雏形——补 printf/malloc/brk + open/read/write/exec
    包装 + crt0；纯逻辑部分（printf 状态机、malloc 堆算法）复用 heap.c 经验宿主单测。~~
    ✅ **v0.27 已部分达成**：cc500 自带极简运行时（syscall3 + malloc/exit/sys\_print），
    用户态已可用 `sys_*` + 共享头 `user_lib.h`；完整 printf/格式化输出仍可作后续增量。

  * **红线（不要走的路）**：不移植 GCC/clang/TinyCC 完整体、不做动态链接（黑洞，吃掉项目余生）。

  * **版本拆分**：27a 汇编器+链接器跑通（手工输入出可执行 ELF 进 FS、被 `run` 执行）；
    27b C 子集前端 → 全链通；28 libc + crt0 + 若干样例程序；29 自举 + 端到端回归通道。
    ✅ **v0.27 已一步达成 27a/27b/29 核心**：直接移植自托管编译器 cc500，guest 内
    `cc500 编译自身 → P1；P1 再编译 → P2；P1==P2` 自举闭环已跑通（`shell ccboot` 命令，
    `[ccboot] … PASS`）。

  * **v0.29 自举仪式验收**：~~`cc.c`~~ ~~编译出~~ ~~`cc2`，`cc2`~~ ~~再编译~~ ~~`cc.c`，产物逐字节一致；~~ ✅
    已达成（cc500 对自身源码是"不动点"，P1==P2 逐字节一致）。
    ~~剩余增量：让编译器支持**命令行指定输入/输出路径**（现为固定~~ ~~`/cc500.c`~~ ~~→~~ ~~`/out.elf`），
    然后演示剧本：agent 经串口/UDP 通道 →~~ ~~`cat > hello.c`~~ ~~→~~ ~~`cc hello.c`~~ ~~→~~ ~~`run a.out`；~~
    ✅ **v0.27b 已完成**：cc500 支持 `argv[1]=输入 argv[2]=输出`（缺省回退固定路径）；
    shell 新增 `writefile <path> <content>`（agent 写源码）+ `ccrun <src> <out>`
    （编译并运行）；guest 内 `writefile /hello.c … → ccrun /hello.c /hello.elf` →
    编译产物被加载运行全链路跑通（test\_serial.sh 用例）。
    ~~⚠️ v0.29 发现（BUG-032）：argv 路径仅对 gcc 版成立、自编译产物静默丢参；~~
    ✅ **v0.30 已修复**：入口桩编组 argc/argv 后，自编译产物（P1）exec 带 argv 也正确。

* **P2 并行项（可插在网络收尾之前/之间）**

  * **record/replay 提前**（简报论点成立：v0.20 "e1000 MMIO 高地址 × 页目录只克隆低 1GB"
    类跨子系统 bug 证明组合爆炸已开始，每加子系统它越便宜）。
    ✅ **方案已细化**（见阶段二「加固」·record/replay 地基候选条目，2026-09-02）：修正此前
    "icount + gdb reverse-debugging"的粗略表述——icount 只定内核、必须叠输入流时间戳化录放，
    gdb reverse-debug 降级为失败后人工定位旁路；公共时钟=QEMU icount 虚拟时钟。

  * **~~DHCP 租期续约（T1/T2 renew）~~**：✅ **v0.28 已完成**（RFC 2131 §4.4.5：
    T1 单播 RENEW / T2 广播 REBIND / ACK 重置 / NAK·超时重新获取；netsock 端口 68
    专用 socket 解决用户 recvfrom"排空"网卡抢先消费应答；test\_net 短租期回归闭环）。
    TCP 状态机仍属"非最小可演进"核心，暂缓。

## 开发原则

1. **每步可运行**：任何一次改动后 `make test` 必须全绿（宿主单测 + QEMU 回归）。
2. **纯逻辑可单测**：与硬件解耦的策略（堆、键盘映射、调度队列）抽成无内核依赖模块，用宿主单测覆盖。
3. **先跑通再优化**：优先正确的功能，再谈性能与安全。
4. **文档随代码走**：每个版本同步更新 changelog / bugs / design。

