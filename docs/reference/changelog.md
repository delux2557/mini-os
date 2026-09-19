# 版本变更日志（Changelog）

> 格式遵循 Keep a Changelog 精神：每个版本列出 Added / Changed / Fixed / Engineering。
> **测试脚本退出码约定（v0.33 起）**：`0` 全绿 / `1` 断言失败（被测代码挂）/ `2` 环境或依赖缺失（缺 qemu/socat/nasm/gcc 等）。目的：让"环境病"显式区别于"代码病"，CI 应将 `2` 标为环境错误而非被测回归。

## [Unreleased] - test-net："sockdemo 进程生成"整行被切碎的假红（与 #194 同一判据收口）

**Fixed**

* **CI 实证**：PR #196（**纯文档**）的 `layer (net)` 红了，缺的是
  `\[shell\] bg 'sockdemo' pid=[0-9][0-9]*` —— 与被 #194 修掉的 `qemu_regression.sh` 是**同一根因**：
  guest 串口行非原子，`[shell] bg '…' pid=N (no wait)` 被并发输出（内核心跳/其它进程）插在中间切成多片，
  整行 grep 永远不中。**文档 PR 不可能引起它** ⇒ 这是 `test_net.sh` 自身的既有判据缺陷，本轮一并收口。
* **修法**（与 `test_serial.sh` / `qemu_regression.sh` 同判据、同复核器）：
  - `check()` 超时后接 `tests/split_line_grep.py`（去壳为字面量后 `-F`；自带**自检**，不过即禁用复核）。
  - 把该断言的正则从 `pid=[0-9][0-9]*` 改为 `pid=.*`：既保住"必须有 pid="的严格性，去壳后又成为
    **可复核的字面量**（`[shell] bg 'sockdemo' pid=`）——含字符类的写法去壳后是它的字面写法、日志里
    不会出现，复核救不回来，故顺手改掉。

**验证**

| 项 | 结果 |
|---|---|
| 直接演示 | ✅ 合成"被切碎"的 net.log：严格 grep **未命中**（假红现场），分片复核**命中**⇒ 恢复 |
| 复核器自检 | ✅ 正例必中／不存在必不中（含 `-F` 方括号路径）；未打印 `[note] …禁用复核` |
| `make test-net` ×3 | ✅ 3/3 全绿 |

---

## [Unreleased] - §6 语言面表补齐：声明子句表 / C 空语句 / `p[i]` / `goto`+标签（清 #187 登记的文档债）

**Fixed（文档-实现一致性）**

* `docs/design/minicc-design.md` 新增 **§6.2d**：把三批**早已实装、却只落在 changelog 与代码注释里**的
  语法补进"语言面唯一事实来源"：
  - **声明子句表**（#157）：`int a,b,*c;` / `char c[3],d;`，全局同形 `int g1,g2[2],*g3;`
    （函数声明子句形态 `int f();` 仍拒）。
  - **C 空语句**（#157）：`;` 合成空 `ND_BLOCK`；`do … while(c);{;}` 同构。
  - **指针下标糖 `p[i]`**（#171，与 #165 同一收口）：脱糖 `*(p+i)`，零新增 codegen。
  - **`goto` / 标签**（#175 host、#182 self）：函数作用域、前向引用回填、越界/重定义编译期报错。
* 按仓库"**清单 ↔ 镜像**"规矩为缺探针的一条补**接受面探针**：`test_minicc.sh` 新增 **`[2b8]`**
  `t_decllist` / `t_gdecllist` / `t_emptystmt`（`p[i]` 与 `goto` 的接受面已由既有 `[2b5]` / `[2b6]`
  与 golden `g12_ptr.c` / `g09_semi.c` 钉住，不重复造）。§6.2d 与 `[2b8]` 互为镜像，改一则必须改另一。

**验证**

| 项 | 结果 |
|---|---|
| 三条新探针（预先用 `hostminicc32` 单跑） | ✅ 均 `rc=0 / minicc: compiled OK` |
| `make test-minicc` | ✅ 全绿（宿主段 `[2b8]` 3 条新增，未破坏既有计数） |

---

## [Unreleased] - 审计工具链"陈旧产物"陷阱：改为按依赖 mtime 重建

**Fixed**

* **`tests/audit/bin/*` 陈旧陷阱**：`test_boundary.sh` / `golden.sh` / `test_diffsynth.sh` 此前只判
  "产物二进制**是否存在**" ⇒ 改了 `tools/cc500/cc500.c` / `tools/minicc/minicc.c` / `tests/audit/harness_src/*`
  后**不重编**，台账/基线仍跑**旧二进制**，把"代码已改"误报成"产物漂移"假红。
  - **实证**：切回 main 后复跑 `test_boundary.sh`，`SPLIT-C-dup*` 三项的 min 列显示 **165/0/0**
    （即 F7 修复**之前**的行为），而工作树里 F7 已在 —— 台账读的是陈旧 `hostminicc32`。
  - **修法**：`build_audit.sh` 新增 **`--check-stale`** 模式（`0`=需重建 / `1`=无需）：依赖集 =
    两份编译器源 + `harness_src/*.c|*.s` + 脚本自身，任一比 `hostcc500`/`hostminicc32` 新即触发重建；
    三个调用方改用它（`test_audit.sh` 本就无条件重建，不动）。
  - 顺带修 `start32.o` 的同款陷阱：此前 `[ -f $B/start32.o ]` 只判存在，改了 `start32.s` 不重编；
    改为 `-nt` 比较。

**验证**

| 项 | 结果 |
|---|---|
| `--check-stale` 三态 | ✅ 无改动→`1`；`touch tools/minicc/minicc.c`→`0`；`touch harness_src/start32.s`→`0` |
| `make test-boundary`（源已 touch） | ✅ 触发重建（`hostminicc32` mtime 前进）且 **36 形态三方逐位全 PASS** |
| 二次跑（无改动） | ✅ 未重建（mtime 不变）、exit 0 —— 验证"该建才建，不空建" |
| `make test-fast` / `make test-diffsynth` | ✅ 全绿（golden / diffsynth 两条调用路径同改） |

---

## [Unreleased] - IPC 挂起可达性：断言由"检查跑过"升级为"验过真实等待者"（+ qemu 门禁假红加固）

**Added**

* `src/app/semhold.c`：**常驻 IPC 等待者夹具**。在**无人会 signal** 的信号量（约定槽 `id=5`）上永久
  `sem_wait` ⇒ 常驻该 sem 的 `waiters[]`；由 `qemu_regression.sh` 用 `bg semhold`（后台 spawn、不等待）拉起。
  - **动机**：#189 的 `[audit] ipc ok: blocked-waiter reachability (checked N)` 若 N 恒为 0，这行只证明
    "检查跑过"（**真空成立**），**不证明**判据能在真实等待者上给出正确结论——而仓库里此前**没有**任何
    "停在 IPC 等待上"的用例（开机的 sem/msg demo 会跑完，`run` 又是同步 wait）。
  - 接线：`src/fs/storage.c` 入 initramfs；`Makefile` 的 `APPS` + 显式 `.elf` 规则。
  - **反证力**：判据把"已登记"误判为"不可达" ⇒ `[audit] ipc FAIL` + `[selftest] audit≠0` 立刻红；
    #188 那类登记簿记失配同样立刻红。semhold 若被唤醒会打 `UNEXPECTED` 并以 `code=9` 退出，便于识别。

**Changed**

* `qemu_regression.sh` 的 selftest 断言由 `\[audit\] ipc ok`（只钉前缀）**收紧**为
  `… blocked-waiter reachability (checked [1-9]` —— 锁 **N ≥ 1**，与前一步 `park IPC waiter` 配套，
  断言的是**非真空结论**。

**Fixed（门禁假红，根因实证 —— 接 #193 明确登记的"留作后续"）**

#193 修好了 `test_serial.sh` 的"整行 grep vs 非原子串口行"假红，并**如实登记** `qemu_regression.sh` 的
`cmd` 有**同一暴露面**、因当时无该处被切的证据而留作后续。本轮实测到该证据（guest 带后台 `sockdemo`/`dhcpd`
每 tick 打 `[net] recvfrom …`，把 shell/app 的整行切碎；实证：`[shell] bg '` + 三行 + `semhold' pid=3` + … +
` (no wait)`），遂一并修掉：

* **分片复核接线**：`wait_after`/`check` 超时后走 `tests/split_line_grep.py`（与 `test_serial.sh` 同判据、
  同复核器；自带自检，**自检不过即禁用复核**，宁可严格失败不要假绿）。
* **去壳为字面量**：qemu 侧模式是 grep-BRE（`\[shell\]`），复核器只吃字面量 ⇒ 调用侧先把 `\x` 去壳再按
  `-F` 复核；本就是正则语义的（如 `[0-9][0-9]*`）去壳后是其字面写法、日志里不会出现 ⇒ **仍不中**，行为同改动前。
* **`.*` 有序分段**（`-F` 模式新增）：`\[deepfork\] CHILD pid=.* grew beyond inherited stack` 这类模式按 `.*`
  切成若干**字面量段**，各段各自做两片复核、**按序命中**才算命中（强度不变：两段都必须在）。**短段（<6 字符）
  拒绝复核**——避免"按序拼接"引入假绿。
* **慢步按步给超时**：`cmd` 支持 `CMD_TMO=<秒>`，对自举编译/深栈/大 ELF/fsdemo 分别给 20–30s。这是"按 step
  分类给余量"，不是整体放宽（避免掩盖真正的卡死）。
* 顺带把 3 条 `.*` 两侧过短的 boot-demo 断言改具体（`msg\] recv.*block` → `msg\] recv pid=.* -> block`），
  使两段都够长、可进复核。

**验证**

| 项 | 结果 |
|---|---|
| `make test-qemu` ×6 | ✅ **6/6** 全绿（同一宿主加固前实测 4/5、5/6）；每轮分片复核命中 1–4 次 ⇒ 加固确在起作用 |
| guest 非真空证据 | ✅ `[shell] bg 'semhold' pid=3 (no wait)`、`[semhold] pid=3 parked on sem 5 …`、`[audit] ipc ok: … (checked 1)` |
| 看门狗 kind=3 | ✅ `kind=3` 命中 **0** 次（semhold 登记正确 ⇒ 不被误判为不可达） |
| `make test-serial` | ✅ 全绿（同一复核器默认路径未变） |
| `make test-fast` / `test-miccboot` / `test-persist` | ✅ 全绿（initramfs 多一个 ~9KB 文件 + Makefile 一条规则，无回归） |

**未覆盖（如实登记）**

* `.*` 分段复核对**短段**（<6 字符）**主动放弃**：宁可不复核，也不接受按序拼接的假绿。
* 分段"按序命中"**不保证两段来自同一逻辑行**（可能是相邻行的巧合）；靠**段足够长 + 自检反例**兜底。
* selftest 的 `(checked [1-9]` 含字符类，**不在**复核覆盖内；其行是内核**单次** `serial_printf`，本轮 6 次
  未见被切（#193 已量化：被切高发区是用户进程**多次** `sys_print` 的行）。

---

## [Unreleased] - test-serial：定位并修掉"整行 grep vs 非原子串口行"的假红（根因实证）

**Fixed**

* **`test_serial.sh` 的 `deep 退出码` 假红：根因找到并修掉**。它此前被归类为"CI 负载抖动"
  （重跑即绿），**实为判据缺陷**：
  - **实证**（CI artifact `build-logs` 里的 guest 串口日志）：guest 侧一切正常——`[user] sys_exit(0) pid=3`、
    `[sched] exit pid=3 name=deep code=0`、`[sched] reap pid=3 name=deep code=0` 全在；但 shell 的整行
    `[shell] 'deep' exited code=0` 被**并发输出切成两段**：`[shell] '` +（6 行后台心跳/收包）+
    `deep' exited code=0` ⇒ `grep -aq "'deep' exited code=0"` **永远匹配不上**。时序 TSV 也记
    `deep 退出码 20249 timeout`（白等满 20s）。
  - **量化**（同一份日志）：**内核记账行 51/51、回收行 60/60 完整**，而用户 shell 行至少 1 条被切
    ⇒ 用户进程打的行会被抢占切碎，内核在 tick/中断上下文打的行不被打断。
  - **修法**：`wait_for` 超时后追加**有界分片复核**（`tests/split_line_grep.py`，成本只在失败路径付）：
    用**同一模式**匹配"行首片段 + 其后 ≤12 行内某行的行首片段"的拼接，命中记
    `[ok] …（整行被并发输出切碎，语义等价）`；**不放松判据**——真正的缺失仍不中。
    复核器自带**自检**（正例必中／不存在的目标必不中），自检不过即**禁用复核**（宁可严格失败，不要假绿）。
  - **验证**：在**真实 CI 失败日志**上命中该行（行 3382 的 `'` + 行 3388 的 `deep' exited code=0`）；
    三个反例（错进程名／错退出码／不存在）均不中；`make test-serial` 全绿。
  - **未覆盖（如实登记）**：`qemu_regression.sh` 的 `cmd` 助手同样是"对累计输出做整串匹配"，
    理论上同一暴露面；本轮**无该处被切的证据**（此前本地观测到的超时项，其目标行在日志中**完整存在**，
    故判为等待窗口而非切碎），故未一并改，留作后续。

---

## [Unreleased] - F5：IP 分片显式拒绝可观测化

**Added**

* `ip_frag_dropped()`（`src/net/ip.c` / `ip.h`）：分片丢弃计数，供分发层日志与宿主单测断言；
  协议层自身不打日志（与 `icmp/udp/netutil` 同风格），日志归 `netsock` 分发处。
  - **复核补：可观测性覆盖到两个方向**。`ip_parse` 有**两个**入向调用者——`netsock` 的 UDP 分发，
    与 **`e1000` 驱动的 ICMP 路径**（`drv/e1000.c` 直调 `icmp_parse`）。只在分发处打日志会漏掉
    ICMP 方向，故把**累计计数**汇入自审计行：`[audit] net: ip fragments dropped: N`
    （**仅观测、不入 `bad`**，同 `fs_owner_violations` 惯例——非 0 表示"有人发分片"，属被防守住的
    输入而非健康失效），并在 `qemu_regression.sh` 的 selftest 断言链里钉住该行。

**Fixed**

* **IP 分片守卫**（`src/net/ip.c` 安全复核 F5）：`ip_parse` 从不读 flags/fragment offset，
  **分片**一律**拒绝**——否则 `udp_parse` 把攻击者自选长度当作完整数据报交给 socket。
* 分发处新增"分片被丢弃"的日志（首次与每 64 次）。

**Why now**：F5 是这条评审链上最后一个已知缺口（`src/net/ip.c:47-52`），静态取证
（`usermode.c:1054` `iov.max>1400`、`netsock.c:41` `plen>NET_RXMAX`、`tcp.c`
`plen>TCP_MAX_PAYLOAD`、`slip.h` `SLIP_MAX` 1600、`NET_ETH_FRAME_MAX` 1518）证实
**所有合法路径都不分片**。

**验证**（本次运行环境：8080 严格 `bind()` 得 `EADDRINUSE`——经复核为 **TIME-WAIT 残留**而非
在跑的服务，见文末订正）：

| 步骤 | 结果 |
|---|---|
| `make`（`-Werror`） | ✅ 干净 |
| `test_ip`（ASan+UBSan，宿主/FAST） | ✅ `pass=31 fail=0`；4 段断言（非首片拒、MF=1 拒、DF 仍受、计数增长不再误增） |
| 判别力反向变异 | ⚠ 注入"分片守卫旁路"⇒ `test_ip` rc=1（4 条 FAIL），还原后 rc=0：变异落盘确认；不依赖 `test-tcp` 环境 |
| `make test-host` | ✅ `pass=20 fail=0` |
| `make test-net test-tcp` | ✅ 两者全绿，`grep ip fragment dropped` 命中 **0 次** ⇒ 零功能回归 |

**附带（不在本 PR 范围）**：`test-tcp` 的宿主 HTTP 口取自 `HTTP_PORT`（默认 8080）；当 8080
不可 `bind` 时它 `rc=2`（环境病）。已用 `HTTP_PORT=8137 make test-tcp` 实测 `rc=0`（双通道绿）
证明问题只是端口；真正接 CI 前仍建议 `test_tcp.sh` 自行挑空闲口（同 `test_tcp_dl.sh` 的改法，见 #192）。

- **复核订正（成因陈述）**：8080 的成因**不是"被反向代理占用"**——`ss -ltnp` 无监听、
  `curl 127.0.0.1:8080` 无应答，而是 `127.0.0.1:8080` 上的 **TIME-WAIT 残留**
  （`SO_REUSEADDR` 可绕过），最可能来自**同一套测试先前运行**留下的宿主 HTTP 服务；
  另有一条 ESTAB 是**出向**连接到远端 `10.96.138.204:8080`，与本地 `bind` 无关。
  **"上一轮残留"比"在跑的服务"更常见**，故端口可搬运性这件事比原陈述的理由更站得住。

* **不变量核查**：`ip_frag_dropped()` 只读，不改变任何返回码语义；分片段在 `ip.c` 入口早 `return -1`，不影响现有合法路径。
## [Unreleased] - 接线：转发器窗口宿主判据升为 FAST 层 `proxy-window`（三个从未跑过的判据）

**Added**

* **新测试层 `proxy-window`**（`tests/test_proxy_window.sh`；`TEST_LAYERS_FAST` 第 6 层，实测 5.7s）
  ——把三个**早已写好却全仓零引用**的转发器判据接进门禁：
  `test_upstream_reliable.py`（上行停等 + 重复包幂等）、`test_upstream_window.py`
  （上行滑窗：突发/乱序暂存/凑齐排出/累计 ACK 跃迁/超窗丢弃 + 恢复）、
  `test_downlink_window.py`（下行滑窗发送端：窗口上限不无界连发/累计 ACK 才推进/
  尾块分片/最老未确认优先重传/CLOSED 在所有下行字节被 ACK 之后）。
* 接线前实测：后两条**独立直跑必失败**（`rc=1 [FAIL] session never opened`，733ms）——
  它们的 proxy 端口是 **argv 入参**，开发时是挂在 `test_tcp.sh` Part A 已启动的代理上跑的，
  从来没有独立入口。本层按各自门面拉起**各自独立**的 proxy 实例（不与会话状态串味）。

**Engineering**

* 防假黄：rc=0 还**必须**出现子判据自己的 `[PASS]` 行，否则视为未真正执行（判 rc=1）。
* 退出码分层：缺 `python3` / 所需端口被占 = **环境病 rc=2**（与 `test_tcp_attack.sh` 同源口径，
  绝不 `fuser -k` 偷杀他人进程）；断言不过 = **代码病 rc=1**。
* **判别力变异实测**（不是"预期能挡"，是摘掉/改坏后看会不会红）：
  ① `UP_WIN 8→1` ⇒ 本层 rc=1，红在 `upstream-window`（补位后未累计推进）；
  ② 把"窗口内乱序暂存"改成"直接按到达序投递" ⇒ rc=1，`upstream-reliable` 与
  `upstream-window` 同时红（精确归因，下行不受影响）；还原后 3/3 复绿。
* 稳定性：同机 3 次 5.653–5.679s（离散 <30ms）；`make test-fast` rc=0（6 层总 16.3s）。
* 端口默认 7793/7794/7795 + 8093/8094/8095，刻意避开业务层在用的 7777/7778/8080。

## [Unreleased] - 接线：dldemo 128KB 下载 e2e 升为 HEAVY 层 `tcp-dl`（并拆掉 8080 硬编码）

**Added**

* **新测试层 `tcp-dl`**（`tests/test_tcp_dl.sh` → `TEST_LAYERS_HEAVY`，实测 24.6–29.6s）
  ——`dldemo` 拉 128KB（远大于 `TCP_RXB` 16KB 接收缓冲）的端到端判据早已写好，但**全仓零引用**：
  不在 `Makefile` 任何目标里，也不在 CI 任何 job 里 ⇒ 与 `proxy-window` 同一形态的"判据静默失效"。

**Fixed**

* **宿主端口不再硬依赖 8080**（接线前实测：8080 严格 `bind()` 得 `EADDRINUSE` ⇒ 该脚本 2/2 红在
  `起 DL HTTP 失败 / OSError: [Errno 98] Address already in use`，与代码无关）：
  - **成因订正（复核）**：这个"被占"**不是**监听中的服务——`ss -ltnp` 无监听、`curl` 无应答，
    而是 `127.0.0.1:8080` 上的 **TIME-WAIT 残留**（`SO_REUSEADDR` 可绕过），最可能来自**同一套
    测试先前运行**留下的宿主 HTTP 服务。残留比"在跑的服务"更常见，故本项可搬运性修复更有必要。
  - `src/app/dldemo.c` / `src/app/httpdemo.c` 的端口常量改为 `#ifndef` 保护，可由
    `-DDL_PORT=` / `-DHTTP_PORT=` 覆盖；`Makefile` 新增 `APP_PORT_DEFS`（`make DL_PORT=8137` 即生效）；
  - `test_tcp_dl.sh` 自动向内核要一个**空闲 TCP 口**并同时用于"宿主监听"与"编进 guest 的
    `DL_PORT`" ⇒ 两端同源，不要求任何环境空着 8080；
  - **转发器 UDP 口刻意保留契约值 7778**（guest 侧 `src/app/tcp.c:21 TCP_PROXY_PORT` 编译期
    硬编码，且 `docs/tcp-session-proto.md` 附录 A 规定了线上一跳）⇒ 该口只做占用预检，
    不自动改选。这一点是我第一版补丁犯的错：把 UDP 口也自动挑 ⇒ 会话开不起来，
    7 项断言全红；改回契约口后一次通过。

**Engineering**

* 失败分类：缺依赖 / 端口被占 = **环境病 exit 2**（沿用 `test_tcp_attack.sh` 口径，不 `fuser -k`）；
  断言不过 = **代码病 exit 1**。
* **判别力实测**（先确认变异真的写进文件再跑，避免"测了未变异代码当证据"）：
  注入"下行发送丢弃尾部 <1500B" ⇒ 本层 rc=1，红在 `len=131072/131072`、`tail=EOFTAIL`、
  `RESULT PASS` 三条上（正是这条 e2e 独属的性质——尾块完整性）；还原代理后复绿。
  另：一次"提前发 MSG_CLOSED（不等在途窗口确认）"的变异**不会**丢字节（包已上线），
  当时误读成"判据失灵"，实为无效变异 ⇒ 换成真丢字节的变异才有结论。
* 稳定性：还原后端直跑 + `make` 目标共 5 次，4 绿 1（变异）红，无端口/时序抖动。

---

## [Unreleased] - 看门狗覆盖 IPC 挂起 + rp_torture 接线（"可判定的挂起"必须看得见）

**Added**

* **看门狗 kind=3：IPC 挂起可达性**（`sched.c` + `usermode.c` 的 `ipc_blocked_ok`）。
  原看门狗只扫 `BLOCK_WAIT`，IPC 等待（`BLOCK_SEM`/`BLOCK_MSG`）一旦簿记失联就**永久挂起、且没有任何
  检查看得见**（#188 修的"交棒吞资源"正属这一类）。新判据与既有 kind=1/2 同一精神 —— **"你等的那个
  东西已不可能让你前进"**，而不是"等太久"：
  - 阻塞在 sem/msg 上却**不在**该对象的等待队列 ⇒ 唤醒只可能来自那个队列 ⇒ **永不可能被唤醒**。
    这是**可判定**的（不依赖对用户意图的猜测），故无误报；
  - 刻意**不扫** `BLOCK_SLEEP` / `BLOCK_KEYBOARD`：它们"等很久"是**合法语义**（由定时器 / 用户输入
    兜底），扫它们只会制造误报 —— 这是"扩覆盖面"最容易踩错的地方。
  - 判据下沉为纯逻辑 `sem_waiter_present` / `msg_waiter_present`（可宿主单测），内核侧统一封装为
    `ipc_blocked_ok(pid, reason, id)`。
* **报频由"全局一次性"改为"按 pid、按一次挂起报一次"**（`wdg_reported[]`，脱困即复位）。
  旧版一旦 dump 过就永不再扫 ⇒ 只管得住**第一次**挂起，后续（或另一个进程的）挂起全部不可见。
* **`[audit] ipc ok` 不变量审计**（selftest 链）：同一判据由自审计**主动查一遍**，并在
  `qemu_regression.sh` 的 selftest 断言链里**钉住** —— 缺陷一出现断言先红，看门狗 dump 只负责给现场。
  行内含 `checked N` 计数（"0 个被检对象"与"查过且都可达"是**不同**结论，故计数必须可读）；
  ⚠ 已知局限：本轮 selftest 现场 `checked 0`（无 sem/msg 阻塞者），故它目前只证明"检查跑过且无违反"，
  若要连同"验过真实等待者"一起证明，需在 selftest 前先 park 一个 IPC 等待者（后续项）。
* **`make test-rp-torture`：接线 `tests/rp_torture.sh`** —— 此前它判据齐备却**未接任何 target**
  ⇒ 写在仓库里但 CI 从没跑过。已入 `TEST_LAYERS` / `TEST_LAYERS_HEAVY` ⇒ CI 新增 `layer (rp-torture)`。
  实测 **≈3.5 min、rc=0**（两次 icount 冷启 + 产物逐字节差分 + tr2sqlite 索引 + 基线巡检 + 现场复原归档）。
* **三个"复现工具"的定位写进 Makefile**（`repro_closer.sh` / `repro_faithful.sh` / `ccboot_check.sh`）：
  它们带 `<kernel.elf> <out.log>` 参数、**没有 pass/fail 判据**，接线成 target 会给出**虚假的绿** ⇒
  有意不接，改为在 Makefile 里注明用途与用法（对应场景的常驻断言已在 `test_socket.sh`(F-0a/F-0b) 与
  `test_miccboot.sh`(P1==P2) 里）。

**Engineering**

* 闸门：`test-fast` ✔（host 20 套件，含 `test_sem` **127/0**、`test_msg` **159/0** 的新判据用例；
  audit / golden sha256 / boundary 36 形态）· `test-serial` ✔ · `test-rp-torture` ✔（新层）·
  `miccboot` **P1 == P2** ✔ · 内核 `-Wall -Wextra -Werror` 零告警。
* ⚠ 本地 `test-qemu` 出现 3 条**超时型**失败（`deepexec` / `ccboot` 两个最慢步骤）：现场显示 guest 健康、
  目标日志行**随后确实出现**，且本轮 `[WATCHDOG]` **零次**（无误报）⇒ 判为**本机计时抖动**
  （正是本轮评审提到的"假失败侵蚀套件可信度"那一类）。该层以 CI 为准（上一轮 CI 为绿）。

---

## [Unreleased] - IPC 生命周期两轴拍板 + 死等待者回收（"F2 该修的那一半"）

**Changed（语义拍板——把长期争论变成台账记录）**

* **IPC 对象 = 内核所有、全局持久**：固定 id（sem 1..15 / msg 1..7）是**全局命名空间**，对象
  **不随创建者退出回收**。理由：命名空间共享 ⇒"创建者已退出"≠"无人再用"（父建子用、父先退
  是正常形态），按创建者回收会抽掉子脚下的对象。
* **与 socket 刻意不同，不可照搬**：socket 是**独占端点**（谁开的谁关 ⇒ owner 校验成立，F-0b）；
  IPC 是**共享通道**（父子各持同一 id、双向可操作正是它的功能 ⇒ 加 owner 校验会废掉 IPC 本身）。
  故"同一栋楼里 socket 修好了、IPC 没修 ⇒ 漏了一半"的说法在**归属**这一轴上不成立。security.md §2
  新增「IPC 对象的两轴拍板」条目，OBS-003 登记页与 §6 自查项同步。

**Added**

* **`sem_reap` / `msg_reap`（纯逻辑）+ `ipc_reclaim(pid)`（内核侧）**，挂在 `terminate_current`，
  对位 socket 的 `netsock_close_pid`（F-0a）。要回收的是**死进程留在等待队列里的条目**——
  持久的是对象、不是死 pid：交棒语义下把死进程当可交付者会**静默吞资源**（sem 丢 token /
  msg 丢消息）。
  - 两侧语义在头文件定死：死**消费者**必须摘（否则 `send_wake` 会先把消息从缓冲 pop 出来再
    交付给一个不会运行的进程 ⇒ 消息蒸发）；死**生产者**必须摘且丢弃其暂存消息（它阻塞中 ⇒
    send 从未返回成功，按"未完成即不生效"）。
  - **现状标注（勿当成已修的可达缺陷）**：今天**无 kill syscall、无跨进程 kill 命令**，而阻塞在
    IPC 上的进程既不在运行（无法出故障）也无自杀路径 ⇒ 本回收**当前不可达**，属**预备防线**。
    现在落地的两个理由：① 它是"任何共享资源都必须在退出路径登记 reclaim"这条约定的首个范本
    （pipe 落地直接照抄这处钩子）；② 语义推理已固定在此，将来加 kill/超时终止时不必重推。
* **`sem_create`/`msg_create` 的 REUSE 诊断**：命中已存在 id 时打 `REUSE (init/capacity 被忽略;
  现存 count)` —— 固定 id 命名空间下这本是一条**静默别名化**路径（两个独立 app 都用 id=1 时，
  后者拿到前者的对象与计数却以为自己新建了），现改为可观测。**这一条是本次唯一可达的实证**。

**Engineering**

* 宿主单测：`test_sem` **116/0**（+`sem_reap` 三态、token 不丢、等待槽可再用）·
  `test_msg` **148/0**（+死消费者不吞消息、死生产者不代发、槽位回收）；host 20 套件全绿。
* 闸门：`test-fast` ✔（audit / golden sha256 / boundary 28 形态）· `test-serial` ✔（含 selftest
  的 `[audit] sem ok` 链）· `test-socket` ✔（F-0a/F-0b 未受影响）· `miccboot` **P1 == P2** ✔ ·
  内核 `-Wall -Wextra -Werror` 零告警。

---

## [Unreleased] - §6.3 拒绝清单改可执行 + 两条防线补断言（"防线可证"起步）

**Fixed**

* **`minicc-design.md` §6.3「明确拒绝清单」有两处写错**（契约文档写反 = 宣告红线不成立）：
  - 仍列 **`goto`** 为拒绝项——而 M14goto 已在 host（#175）/ self（#182）两侧实装；
  - 「隐式指针转换」**未分方向**，与 §7.3「`type_eq` 允许右值 int 赋给指针」自相矛盾。实测：
    `int* p; p = 5;` **被接受**（有意放宽，用于承接 `xmalloc` 的 brk 地址），反向
    `int a; int* p; a = p;` 才被拒。已按方向订正为「指针→整型 方向的混赋」。

**Added**

* **`test_minicc.sh` `[2a5]`：§6.3 的可执行镜像**——清单每一条都有探针，断言"编译期拒绝且不产出
  产物"（16 条），另含 `int → ptr` 的对照探针（防将来有人"顺手把整条标成拒绝"）。块首注明
  **改清单必须同步改本块（反之亦然）**。宿主段数 122 → **139**（+17 探针，全绿）。
  - 为什么要它：该清单是**语言契约**，而上面两处错误存活了很久无人发现——纯散文的契约无法被
    自动校验；钉成可执行后，无论"改实现"还是"改清单"，任一侧漏改都会立刻报错。
* **`test_serial.sh`：两条防线日志补断言**（此前全仓 `tests/` 对这些串零引用，"防线存在"≠"防线可证"）：
  - `[ssp] kernel stack guard randomized` —— 钉住 SSP 启动随机化确实生效；
  - `[syscall] pid=N masked syscall M` —— 沙盒用例此前只断言 app 自己的 `verify OK`，而
    **掩码若失效、"成功返回"也可能凑出 OK**；现直接断言内核拦截日志本身。
  - 另**诚实留档**两条故意未断言者（`[STACK-GUARD] canary stomped` 属内核栈 panic 路径；
    `[WATCHDOG] … STALLED` 需人为制造挂起），并注明后者本身是待收口项——不假装覆盖。

**Engineering**

* 闸门：`test-minicc` 宿主 **139/0** ✔ · `test-serial` ✔（两条新断言实跑通过）·
  `test-fast` ✔ · `miccboot` P1 == P2 ✔。
## [Unreleased] - 同作用域重声明受控拒绝（安全复核 F7：minicc 与 gcc 对齐）

**Fixed**

* **minicc 静默接受 C 必须拒绝的重声明**（host 与 self 双侧同源根因）：`int a=1,a;`、
  `int a;int a;`（同一块内）、`int f(int a,int a)`（形参表内）三族都直接 `sym_add`，
  而 `sym_find` 是"从后往前、最近声明优先"⇒ 三族**全部编译通过，产物语义与 C 不可对应**：
  `int a=1,a; return a-1` 读到未初始化的后一个 `a`（实测退出码 1），同族用例实测**直接崩溃**
  （台账观测值 `165` = 信号归一哨兵）；C 对这三族都是编译期错误。
  这正是本仓 §6.3 的"**绝不产出坏码**"红线所针对的形态。
  - 改法：新增 `scope_floor`（当前作用域在符号表中的下界）+ `sym_check_dup`，
    在**局部声明子句**与**形参**两处 `sym_add` 前受控报错（`redeclaration in same scope` /
    `duplicate parameter name`）；`block_stmt` 进出保存/恢复下界。
  - **不误伤合法遮蔽**：嵌套块遮蔽外层局部、局部遮蔽同名全局函数（BUG-035 依赖）仍合法，
    由台账 `shadow-nested` / `shadow-gfun` 与 `mc_matrix` 的 `M7d-*` 哨兵守着；`self` 侧按
    同一消息同一路径镜像（子集内无 `void`/三目，故拆成两个 `int` 助手），由 `[2b7]` 的
    `p23_dup_clause`/`p24_dup_param` 钉断言「双实现同拒 + `error: ` 前缀段等值」。
  - ⚠ **一处与 gcc 的已登记分歧（#187 复核时发现并订正）**：`int f(int a){int a;…}` 本实现
    **接受**，而 gcc（c89/c99/c11 实测）报 `'a' redeclared as different kind of symbol`。
    初稿把它与前两类并列为"C 合法遮蔽"是**误判**（gcc 视为重定义）。保留该宽松口径是有意的：
    **cc500 同宽**（实测两者都返回 7）⇒ 只改 minicc 会让双编译器互相分叉，而 cc500 侧须按 M
    里程碑走"旧语法产物零变化"证明才能动。已登记为 `DIV-param-shadow`（锁 `REJ/7/7`，唯一
    离群者是 gcc）；复核并实测：把口径**改严后 `minicc_self.c` 与 golden 全 13 例仍全编过**
    ⇒ 该分歧**可消除，但当前不划算**（改单侧得不偿失）。
* **本 PR 自身也按 #186 的"清单 ↔ 镜像"规矩同步**：两类新增拒绝已入 §6.3（并写明"重声明 ≠
  遮蔽"只适用于嵌套块/遮蔽全局函数两类；形参-体顶层同名单独登记为上述 `DIV-` 分歧），
  `[2a5]` 补 3 条探针 `rl_dup_scope` / `rl_dup_two_stmt` / `rl_dup_param` ⇒ 宿主段 141 → 144。
* **仍存的一处文档债（本 PR 不做）**：§6 语言面表缺 #157（声明子句表 + C 空语句）、
  #171（`p[i]`）、#175（`goto`/标签，self 镜像见 #182）三批已实现语法；§6 自称"语言面
  唯一事实来源"，这批当时只落在 changelog 与代码注释里。补它们须按同一规矩为每条配
  `[2a5]` 风格的**接受面**探针（清单与镜像同改），是独立一件事。

**Engineering**

* 新增 **6 条三方台账行（28→34 形态）**：`SPLIT-C-dupclause` / `SPLIT-C-dupblock` / `SPLIT-C-dupportyr`
  （本轮把 minicc 拉回与 gcc 一致 ⇒ **cc500 成为离群者**，其静默接受显式登记为 `0`；
  不在本 PR 动 cc500——它是单遍无类型面设计，改动需按 M 里程碑纪律走产物零变化证明）；
  `SPLIT-G-globdup`（文件作用域 `int a,a;` 按 C 合法，两编译器均拒——既有更严口径，登记防误导）；
  `shadow-nested` / `shadow-gfun` 两条**过度收紧哨兵**（期望 `0/0/0`）。
* `mc_matrix.sh` 加 `M7d-*` 五钉（3 拒 + 2 哨兵），`[2b7]` 加 `p23/p24` 双实现同拒钉。
* 实测判别力：去掉守卫后三条 `SPLIT-C-dup*` 的 minicc 列由 `REJ` 变 `OK(0)` ⇒ 台账即门禁。

---

## [Unreleased] - 符号/补丁/标签表改 brk：消除两条近在眼前的自举容量悬崖（产物 −55.3 KB）

**Fixed**

* **三条静态容量天花板已贴边**（`minicc_self.c`）：符号表 / 补丁表 / 标签表此前是**静态数组**
  （#172 只把节点池与名字池改成了运行时 brk，这三条被漏在外面），于是容量按字节全量内联进
  产物（实测 **29 B/符号位、12 B/补丁位、12 B/标签位**；旧配置 256/2048/2048 合计 **56,576 B
  = 产物的 40%**），"抬容量"等于给内核加体积。而自举源的真实需求已达 **238 符号 / 1,973 补丁
  / ≈1,250 标签** ⇒ 余量只剩 **18 / 75 / ~800 位**。
  - **为何算悬崖而非"余量偏小"**：#182 goto 镜像**一个特性就吃掉 6 个符号位**（232→238）；
    按此实测速率，符号表撑不过 ~3 个特性 PR、补丁表撑不过 ~2 个。越界症状是自举静默断掉
    （`symbol table full` / `too many references`），由 CI 的 miccboot 层抓出。
  - 改法沿用 #172：三表改 brk + 容量抬到与 host 对齐（`SYM_MAX 512` / `PATCH_MAX 4096` /
    `LAB_MAX 4096`）⇒ 产物 **142,638 → 87,349 B（−55.3 KB）**，余量变为 2.1× / 2.0× / 3.3×。
  - **一处必需的非显然改动**：brk 内存不保证为零，而 `finish()` 用 `lpos[lab] < 95` 识别
    "标签已分配但从未发射"，依赖的正是静态数组的隐式零初始化 ⇒ 分配后必须显式清零
    （新增 `zero_ints`/`zero_chrs`）；否则读到垃圾 ≥95 会跳过该守卫、把补丁写向野地址。
  - **一处已计入的代价**：改 brk 后每个表引用点多占 1 个补丁位（指针全局需地址补丁，静态数组
    可直接寻址）⇒ 补丁需求 1,973 → 2,057（+84，+4%）；符号只 +2（两个清零点）。

**Docs**

* 台账订正（`minicc_self.c`「自举容量」段 + `minicc-design.md` §7.3）：天花板由"四条"改为七条
  并给出各自现价与余量；订正旧文档"`PATCH_MAX=4096`、`strpool[4096]` 维持静态（合计约 8KB，
  占比已小）"——该处既把 host 的 `PATCH_MAX` 当成了 self 的，也完全漏掉三张表的 56.6 KB 占位；
  并记明剩余静态占用 **≈25 KB**（循环帧栈 16,384 / strpool 4,096 / goto 表 4,032 / tok 256）
  与"池式峰值只能实测 ⇒ miccboot 是那道闸"。

**Engineering**

* 闸门：`test-miccboot` **P1 == P2 逐字节一致** ✔ · `test-minicc` 宿主 122/0 ✔ ·
  `test-fast` ✔（audit / golden 双编译器 sha256 逐行一致 / boundary 28 形态）·
  `test-cc500` 103/0 ✔ · `test-diffsynth` ✔ · `test-diffsynth-guest` 12/12 ✔。
* 两处独立验证：① 峰值口径用"把 host 的容量宏逐步压小、看何时撞上限"反求，同一把尺子量了
  6 个历史版本（224 / 232 / 232 / 232 / 238 / 238 ⇒ 符号位只在 #182 跳过一次）；
  ② "容量真的放开了吗"用 **400 全局符号 + 400 处引用**的源实测：改前 self 拒
  （`symbol table full`），改后编过。

**补记（#183）**：`21d49f6` 修的 `nnargs` char 截断（257 实参回绕成 1 ⇒ arity 门被静默绕过 +
调用点漏 1 KB 栈；128~255 实参则被误拒）当时只落在代码注释与 `[2b7]` 三钉里，未进本日志，此处补记。

---

## [Unreleased] - self 编译器帧大小 char 截断修复（#182 收口：miccboot P1 == P2）

**Fixed**

* **`nnlocals` 误用 `char` ⇒ 局部帧 ≥256 B 被按 256 取模截断**（`minicc_self.c` 内存池段）：
  `gen_func` 以 `save32(frame_patch, nnlocals[n])` 回填 prologue 的 `sub esp,imm`，而 `nnlocals`
  与 `nkind/nty/nbty/nvkind/nnargs` 一并被定为 char，注释断言"值恒 <256"——但它是**局部帧字节数**
  （上限 `LOCAL_BYTES_MAX = 4096`），该断言不成立：328 → 72、256 → 0。
  - **为何长期静默**：截断前自举源自带函数没有一个帧 ≥256，缺陷没有显形条件；本分支给 `stmt()`
    新增 label 探测的 `char bb[256]` 后帧首次到 328（328 & 0xFF = 72），才由 miccboot 的逐字节
    比对暴露。编译期无警告、功能面不复现（帧内写入未越界到他人），属**只有产物比对才可见**的一类。
  - 修法：`nnlocals` 改 `int*`（4 B/节点，分配同步 `NMAX * 4`），注释订正该字段不在"恒 <256"之列。
  - 帧计算本身（`cur_frame`/`bytes_of`）与 host 一直一致，故这是分支上**唯一**一处 P1/P2 差异。

**Engineering**

* 宿主等价 A/B（免 QEMU）：最小用例 `int f(){int bb[64];...}` 帧 host=256 / self=0 → 修后双方 256；
  打回原状即复现 `[diff] off=115975 a=1 b=0`（host `sub esp,0x148` vs self `sub esp,0x48`）。
* 闸门：`test-miccboot` **P1 == P2 逐字节一致** ✔（修前 FAIL）· `test-minicc` 宿主 119/0 ✔（含 [2b7]）
  · `test-fast` ✔（audit/golden/boundary 28 形态）· `test-cc500` 103/0 ✔ · `test-diffsynth` ✔ ·
  `test-diffsynth-guest` 12/12 ✔。
* 本类缺陷的常驻门禁**已存在**（miccboot 在 CI 全链内，且正是本次抓出它的那道闸），故不另加门禁；
  若后续把 `char bb[256]` 从自举源中移除，该处覆盖会随之消失——届时需在 gate 层另钉一条帧 ≥256 的用例。

---

## [Unreleased] - diffsynth：cc500 目标解禁 F_SUGAR（复合赋值/自增自减入网）+ 覆盖断言

**Changed**

* **cc500 目标解禁 `F_SUGAR`**（`gen.c` 的 `CAPS_CC500`）：
  复合赋值（`+= -= /= %=`）与前后缀 `++/--` 入 cc500 差分网。原台账记"cc500 无 ++/-- 与复合赋值"
  为**过时表述**——cc500 的 M4（`+= -= *= %=`）/M8（`++ --`）/M9b（`/=`）与 minicc V3b 早已实现；
  生成的左值仅限 int 全局/局部（`v%d`/`G%d`），不触 char/数组/指针，无 UB、值域收敛。
  能力台账注释同步订正（F_ARRAY/F_PTR/F_CHAR 仍关，各自门槛不变）。

**Added**

* **`F_SUGAR` 覆盖断言**（`test_diffsynth.sh`）：
  cc500 目标样本必须真的采样到语法糖形态，否则判红——防"解禁"成为空洞 PASS（同 GG2 死钉教训）。

**Engineering**

* `make test-diffsynth` ✔：minicc 5 seed + cc500 3 seed 全过，acceptance/退码差分 0；
  `F_SUGAR 覆盖` ✔（11 个样本含语法糖形态）；gcc 参考侧纪律（无挂/信号/确定性错）保持。

---

## [Unreleased] - self 侧内存安全门禁（UBSan）+ 构建依赖缺口修复

**Added**

* **`[2b7]` self 编译器内存安全门禁**（`tests/test_minicc.sh`）：hostself 以
  `-fsanitize=undefined -fno-sanitize-recover=all` 构建，跑形参密集语料 6 例，断言
  ①全部 `compiled OK` ②零 UBSan `runtime error` ③产物与 hostminicc **逐字节一致**。
  - 动机：安全复核 F6（char 池字段当 `int*` 传出去 ⇒ 4 字节错位写）属"**三面皆不报**"类缺陷——
    编译期无警告（gcc 视角是合法 int* 写）、功能上常"恰好正确"（故 miccboot 的产物逐字节比对
    不会红）、且 self 侧此前不在任何 ASan/UBSan 面内 ⇒ 可长期静默存活。三面皆不报的缺陷必须
    补一面显式门禁。
  - **实测判别力**：换回 F6 前源 ⇒ 6/6 各报 1 次 `store to misaligned address`（`minicc_self.c:556`）；
    修复源 ⇒ 6/6 清零且产物同 hostminicc。
  - 入口路径不加写 `/` 依赖：`minicc_self.c` 的 `main` 固定读 `/minicc.c` 写 `/out.elf`，而 CI runner
    无 `/` 写权限（既有 `[2b4]/[2b5]` 因此 SKIP）⇒ 本块用**路径重写副本**，并断言两个路径字面量
    各命中 1 处（源一改即报错，覆盖不会静默失效）。

**Fixed**

* **app 对象不参与头文件依赖跟踪**（`Makefile`）：`-include $(BUILD)/*.d` 只覆盖顶层对象，而 app
  对象的 `.d` 在 `$(BUILD)/apps/` 下 ⇒ **改头文件（如 `src/app/user_lib.h`）不会重编 guest app**，
  本地增量验证会静默使用旧 app 二进制（"假绿"）。补 `-include $(BUILD)/apps/*.d`。
  - 实测：修复前改 `user_lib.h` 后 `make` 不重编 sandboxdemo（A/B 实验因此被静默作废）；修复后
    正确重编；内核侧 `src/net/netutil.h` 的依赖跟踪不受影响。

**Engineering**

* `make test-minicc` ✔（宿主 119/0，含新块 6 例）· `make test-fast` ✔ · 构建 `-Werror` 干净。

---

## [Unreleased] - 术语中性化与历史重写（含协作者操作须知）

**Docs**

* **术语中性化**：本轮审计相关的文档与注释中，一批以器物/口语比喻为主的表述已统一改为中性技术
  表述（规范化后的词表：`主动认领`、`本次改动`/`本次改动范围`、`三处改动`/`四类转义`、`续作`、
  `单项`、`全局改动`、`第一项改动`/`某次改动`/`各项改动`、`失效断言`、`静默`、`误导`）。涉及
  `v2-c-kernel/Makefile`、`tools/minicc/diffsynth/gen.c`、`docs/design/minicc-v3-后续任务.md`、
  `docs/reference/changelog.md`、`tools/minicc/minicc_self.c`（注释）。仅文本，无语义变化。
  - **判定为保留**（既有仓库风格或标准术语，非本轮引入）：仓库长期使用的复现/事故类用语、
    `黑盒`（black-box）、`fork 炸弹`（fork bomb）。

**Changed（操作记录——影响所有协作者）**

* 为使上述表述**不出现在历史中**，已对全部提交执行 `filter-branch` 重写（提交信息 + 文件内容双侧过滤）
  并强推 `main` 与 43 个分支：**自 2026-09-16 起的提交 SHA 全部变化**。
  - **协作者请重新克隆**（或 `git fetch origin && git reset --hard origin/main`）——旧克隆的历史已分叉；
  - 窗口内已合并 PR 的 merge SHA 不再可达，其页面上的 commit 链接会失效；
  - **唯一残留位置**：GitHub 托管的隐藏引用 `refs/pull/*`（173 个）仍指向重写前提交，平台不允许
    改写或删除（`! [remote rejected] … (deny updating a hidden ref)`），需 GitHub Support 侧清理；
  - 强推当次的 `guard-lint` 因 `GUARD_BASE = push.before` 指向已被重写掉的提交，按设计拒绝给出结论
    （exit 2，非回归）；后续事件取 `pull_request.base.sha`，不受影响。

## [Unreleased] - 名字池越界守卫 + 动态化 + 定容依据勘误（#172 后续）

**Fixed**

* **`stradd` 无越界守卫 ⇒ 静默写穿内存**（`minicc_self.c` 名字池）：`strtab[nstr] = …` 没有任何边界
  比较，池满即写穿相邻内存，**报出来的却是与真因无关的 `too many nodes`**。这不是理论风险：
  #172 的初始误判正源于此，本轮镜像分支（`feat/minicc-self-goto`）的 label 探测又踩了同一个坑
  （每条以单词开头的语句都多入池一份名字，把已用到 90% 的池子撑爆，排查全程被节点池误导）。
  改为越界受控 `fail("strtab full")`。**实测判别力**：把 `STRTAB_CAP` 改小到 8000 ⇒ 稳定报
  `minicc: error: strtab full [r] @34606`，不再伪装成节点池问题。

**Changed**

* **勘误 #172 的定容推导**：早期记录的"密度 0.269~0.315 节点/字节、宿主 minicc 的密度只有一半"
  来自一次**被污染的实验**——为测 `NMAX` 用的 `sed s/13312/N/g` 把当时同样被误改成 13312 的
  `strtab` 一并放大了，于是"16384 失败"其实是名字池溢出。实测真值：AST 节点峰值 **8,809**
  （源 66121 B ⇒ 0.133/字节；宿主 minicc 同源 8,317 ⇒ 两者只差约 5%）、名字池用量 **18,496**
  （0.28 字节/源字节，当时已是静态池 `[20480]` 的 **90%**）。源码「自举容量」段已重写并留档出错原因。
* 名字池由静态 `strtab[20480]` 改为运行时 brk 分配（`STRTAB_CAP = 131072`，满源按 0.28 需 ≈36 KB）
  ⇒ 与节点池同样"容量与产物脱钩"，自举产物 **155,215 → 134,881 B**（那 20 KB 静态池不再内联进产物）。

**Added**

* FAST 层新钉 **`S2e`**：名字池容量 ≥ 源体积 × 0.5（由实测 0.28 反推，保底 ≥1.8× 余量），
  防"容量又被改小 / 源涨过头"这类静默回归——与该组其余钉同一分工：静态可算的在 FAST，只能实测的靠 miccboot。

**Engineering**

* 闸门：`make test-fast` ✔（含新 S2e 与 S2c 产物 134,881）· `test-miccboot` **P1 == P2 逐字节一致** ✔ ·
  golden 13 例零漂移 ✔ · boundary 28 形态 ✔ · cc500 启动不动点 50,173 B 保持 ✔。

---

## [Unreleased] - 自举不动点 P1==P2 修复：容量重标定 + 节点池动态化 + miccboot 入 CI（#172）

**Fixed**

* **minicc 自举不动点 P1==P2 在 main 上已断（#172）**：`miccboot` 实测 `minicc: error: input too big`
  → `P1 != P2 FAIL`（`len=0`：源一个字节都没读进去）。根因是自举源自带的两道上限按 2026-09-05 的
  40293 B 源定下（`in_len >= 60000`、`NMAX = 8192`），源一路涨到 62199 B 却从未重标定：
  2026-09-16 `23217bc` 越过输入上限 ⇒ 自举自此断掉。抬掉输入上限后暴露第二道 `too many nodes @61295`
  （`NMAX` 只差 123 个节点）。**判决性实验**：只抬这两个常量即恢复 `P1 == P2` 逐字节一致。

**Changed**

* **自举容量重标定，并把"容量↔内核体积"的耦合解开**：
  - 输入：`IN_CAP = 131072` / `IN_LIMIT = 126976`（差一个读块余量，顺带修掉"上限与容量同值 ⇒
    `in_len` 刚过上限时那次 4096 读越界写缓冲"的隐患）；宿主 `minicc.c` 同构抬到同值（它是 P1 的
    构建者，先撞墙就没人能构建 P1）。
  - 节点池：15 个并行数组由**静态数组改为运行时 brk 分配**。关键发现：minicc 不分离 `.bss`，
    零初始化全局数组按字面量**全量内联进产物**（42 B/节点）⇒ "节点容量"直接等于"内核里那份自举
    编译器的体积"，抬容量就要给内核加体积、于是谁都不动它、自举就断在那儿。动态化后容量与产物脱钩：
    自举产物 **497,496 B → 155,215 B（-69%）**，`NMAX` 可一次给到 49152（按实测 0.133 节点/字节，满源约需 17k ⇒ 余量 ≈2.9×）。
  - 产物：`code_cap` 860000 → 500000（≈135 KB 产物的 3 倍余量）。
  - 定容依据**改用自举源自身的实测**：AST 节点峰值 8,809（源 66121 B ⇒ 0.133 节点/字节）、名字池用量 18,496（0.28 字节/源字节）。
    > **订正（同轮后续提交）**：本节初版曾写"源 65052 B 时 16384 不足、20480 够 ⇒ 密度 0.269~0.315，
    > 宿主 minicc 的密度只有一半"。那组数字来自一次**被污染的实验**——批量替换 `sed s/13312/N/g`
    > 把当时同样被改成 13312 的 `strtab` 一并放大了，于是"16384 失败"其实是**名字池溢出写穿相邻内存**，
    > 报出来的却是 `too many nodes`（与真因无关）。真值见上；名字池也已补越界守卫并动态化，见下条。

**Added**

* **`miccboot` 纳入 `TEST_LAYERS`（HEAVY）+ `test-miccboot` 目标**：这条被文档写成"最终裁判"的核心
  不变量**此前不在任何 CI 层里**，于是"源涨过自举容量 ⇒ 自举静默不过"无人发现。核心不变量必须有
  常驻门禁，这是根因修复（否则修好还会再烂一次）。
* **FAST 层自举容量钉 S2a/S2b/S2c/S2d**（`mc_matrix`）：常量一律**从自举源里读**（单一事实源，
  改源即改门禁、不手抄数值），断言 ①读块余量关系 ②源体积 ≤75% `IN_LIMIT` ③**实测产物** <90% `code_cap`
  ④节点池未被回退。S1 同时改为复用该产物供 ③ 实测（零额外成本）。

**Engineering**

* 闸门：`make test-fast` ✔（含新 S1/S2 组；cc500 不动点 50,173 B 保持）· `test-minicc` 107/0 ✔ ·
  `test-cc500` 103/0 ✔ · `test-diffsynth` ✔ · `test-miccboot` **P1 == P2 逐字节一致** ✔ ·
  golden 12 例零漂移 ✔ · `guard-diff` ✔。
* 双实现等价：`hostminicc` 与 `hostself`（gcc 直编 `minicc_self.c`）对新源产物**逐字节相同**（155,215 B）
  ⇒ "两份实现语义一致"这一 P1==P2 前提可在宿主侧单独验证，不必等 guest 慢层。
* **第 N 次同类实证：没跑的门禁会掩盖"早就不成立"**。此前是 `GG2` 死钉、`t_gundef`、`t_ginit_hexe`，
  这次直接是核心不变量本身——诊断口径已写入本文件 Engineering 段，作为加/删门禁时的对照清单。
* 顺带推进 `docs/design/minicc-v3-后续任务.md` 任务 7（arena 瘦身）：原定"把 8192 收到 peak+裕量"，
  本次以"动态化"达成且更彻底（容量与产物解耦），剩余字段瘦身/密集竞技场仍留待后续。

---

## [Unreleased] - cc500×minicc 指针访问语法双向对称（#165 / M13）

**Added**

* **cc500 一元 `*` 解引用 / `&` 取地址（M13）**：原 `accept("*")` 仅二元乘与声明层，unary
  deref 从未进文法——cc500 程序读不了字符串/指针目标（`if(*p==5)`、`*(s+1)`、`*p=…` 一律
  `error at *`），字符处理类教学示例在 cc500 侧整体不可写；`&` 同缺（票面未提，本轮复核补出，
  而票面自己列的「`*p=` 写」验收正需要它）。因左值表示 = **地址驻留 %eax**，deref **零发射**
  （`promote` 装载后指针值即被指左值地址）；`&` 同理直接返回地址值，非左值（type 3）拒。
  旧语法产物字节零变化（`*`/`&` 位于操作数首位原本必报错，无既有合法源受影响），
  自举 P1==P2 逐字节保持。
* **minicc 指针下标糖 `p[i]` ≡ `*(p+i)`（#165 对称的另一半）**：原 ND_INDEX 只服务数组声明
  （`TY_ARRAY`），`char *s; s[1]` 报 `type mismatch in assignment [[]`。现于 `postfix` 层脱糖，
  复用既有 `ND_ADD` 指针缩放 + `ND_DEREF` 取宽，**零新增 codegen**；非指针带下标拒。
  `minicc.c` 与自举源 `minicc_self.c` 同步改（P1 可构建钉保持）。

**Changed**

* `mc_matrix` 新增 **G7 组**（#165 验收义务「对称矩阵」的常驻硬门）：新断言器 `BOTH` 把
  **同一份源码**在 cc500 与 minicc 两侧各编各跑、逐项比对——解引用/下标的读与写、`*(s+k)`、
  取址后解引用、`&` 非左值双拒，共 8 钉。单侧钉无法区分「两边都对」与「两边各错一半」，
  故「对称」只能这样证。
* `mc_matrix` 新增 **G7-有意分歧-int宽**：显式锁住 cc500「无类型面」导致的宽度差（同一份
  `int *p` 源码，cc500 按 char 宽返 7 / minicc 按 int 宽返 8）。把"已知有意分歧"钉成契约，
  任一侧漂移即红；对称组本身只用 char 宽形态，避免把分歧算成对称。
* `verify_findings` **E19 由占位转正**：串内容从"只能压 golden 哈希"升级为**值通道直读**
  （`*s`/`s[1]` 解引用读出），E 套件 21 条。
* `golden` 语料新增 `g12_ptr.c`（deref 与下标的读、写各一次；exit 列 37，cc500/minicc/gcc
  三方同值）——cc500 deref 路径的首个 golden 观测。清单**声明后重生成**，唯一变更 = 新增该行，
  g01~g11 哈希逐字节零漂移。`g10_esc.c` 仅改注释（说明该文件写于无 deref 时代），哈希不变。
* `diffsynth/gen.c` 仅订正能力台账注释：F_ARRAY/F_PTR/F_CHAR/F_SUGAR **仍关**——各有独立门槛
  （无数组声明 / `npa=has(F_PTR)&&na>0` 依赖 F_ARRAY / 无 char 数组 / 无 ++-- 与复合赋值），
  与 deref 无关；本轮解禁的是**语法接受面**，不是这一族的生成网。
* `docs/README`「cc500 方言边界」+ `tools/cc500/README.md`（M13 行 / 语言快照 / 已知限制 / 验证）
  补「指针访问」条目与**有意差异**声明。

**Fixed**

* **`mc_matrix` 死钉（#166 遗留、main 上既有）**：G5 组在 `GG2` **定义之前**就调用它 → bash
  只往 stderr 丢一行 `GG2: command not found`，而脚本无 `set -e`、调用方也不查 stderr ⇒
  `G5-cc500-双反斜字宽` 这条钉**从未执行**却仍报「全绿」（#166 加的判别力钉一直是死的）。
  修：定义前置 + 新增**死钉自检**——整脚本 stderr 收进日志，末尾检出"命令未找到"即判红，
  诊断经 fd 3 原样回放不丢失。同类"断言函数定义滞后"从此不再静默。

**Engineering**

* 五闸实测：`make test-fast`（host/stack/audit/golden）✔ · cc500 层宿主 103/0 全绿
  （含新增 M13 五钉）· minicc 层 107/0 全绿（含 mock 白盒 101/0）· diffsynth（含 cc500 目标）✔ ·
  **cc500 启动不动点 P1==P2（50173B）逐字节保持** · guard-diff ✔。
* 复核方法论沉淀：**原型先落地再评审**——本次先在临时 worktree 把三处改动（45 行）跑通全部门禁，
  再据此给出范围建议，避免了"先评估后返工"。同时再次印证：**已绿的东西可能一直绿得错误**
  （`mc_matrix` 死钉靠"没跑"变绿，与 `t_gundef`、`t_ginit_hexe` 同族）。
* **改 minicc 侧的验证替代**：`minicc_self.c` 是手维护孪生源，其自举不动点由 `miccboot` 承载，
  而该检查**当前不可用**（见下行，非本 PR 引入）。替代证据 = `test_minicc.sh` 新增 `[2b5]` 块：
  gcc 直编的 hostself 与 hostminicc 对 p[i]/*p/&x 四种新形态产物**逐字节一致**——两份实现的
  行为等价正是 P1==P2 的前提（P1 由 hostminicc 产物链推得）。

**发现（不在本 PR 范围，建议另开票）**

* **minicc 自举不动点在 main 上已断（pre-existing）**：`minicc_self.c:1546` 自带输入上限
  `in_len >= 60000`，而该源文件本身已达 **61487 字节**（基线 24ef9c3）——即 P1（由该源编译而来）
  **读不下自己的源**。实测 `tests/test_miccboot.sh`：`minicc: error: input too big` →
  `[miccboot] P1 != P2 FAIL`，基线与本分支**输出完全一致**（本 PR 未改变其成败）。
  该检查不在 `TEST_LAYERS` 内、CI 不跑 ⇒ 与本期修的 `GG2` 死钉同族：**"没跑"掩盖了"早就不成立"**。
  本 PR 对 minicc_self.c 的改动使其 +712 字节（62199），未改变结论（原本已越限）。
  建议：单开一票处理（上限提升 + 缓冲余量核算 + miccboot 纳入 CI 层），不在 #165 内混做。

---

## [Unreleased] - cc500 盲区修补：标签终检失效 + 入口选错（#161/#162）

**Fixed**

* **cc500 `lbl_end` 标签终检从未生效（BUG-076/#161）**：标签记录锚点=名字 NUL 位置，而
  终检从记录**起点**读 flag（读到的是名字第二字符）→ `'u'` 判定恒假，`goto` 未定义标签
  一路静默放行、产出跳向未知地址的产物。修法：先扫到 NUL 再查 flag/target。
  旧测试 `t_gundef` 的"应拒"长期由 `done:;` 空体分号走 expression 错路**意外绿**，
  #160 合法化空语句后伪装脱落、CI 如实报案（本轮审计锁反向价值的第二个实证）。
* **cc500 入口选错：main 之前的函数体夺走入口（BUG-077/#162）**：入口 stub 的 call rel32
  只回填「首个函数体」（自举需要：cc500.c 首函数是 `cc500_main` 且无 main），故
  `int f(){...}int main(){...}` 实际执行 f、main 成死代码——`f()-1` 类程序一律 exit 1，
  一度被误诊为"函数调用结果进算术就算错"。修法：对名为 `main` 的函数体**再**回填一次
  入口（无 main 时保留旧契约），入口=main 对齐 minicc/gcc 语义。

**Changed**

* `test_cc500` 新增 T 系列六钉（`t_gosemi`/`t_godef` goto 两侧症状对立、`t_entry1..3`
  票面三形态）；`t_gundef` 期望回正 rc=1（**这次真由 lbl_end 拦**，非体分号错路）。
* `mc_matrix` 新增 G4 组钉：编译 rc×2（goto 未定义拒/定义在后不误伤）+ 运行值×4
  （票面三形态 + 入口判别钉 `int f(){return 7;}int main(){return 0;}`，旧码必红）。
* `golden` 清单**声明后重生成**：唯一变更 = `g03_call`——全库唯一"函数体在 main 之前"的
  语料，由**静默错编译**转为可运行正确例（rc=0）；minicc 及其余 6 例零漂移。
* `docs/reference/bugs.md` 补 BUG-076/BUG-077 条目。

**Engineering**

* 五闸实测：`make test-fast`（host/stack/audit/golden）✔ · cc500 层宿主 92/0 全绿 ·
  minicc 层 107/0 全绿 · diffsynth（含 cc500 目标）✔ · 启动不动点 P1==P2（48804B）·
  guard-diff ✔。
* 教训沉淀：产物哈希基线只锁"字节一致"、**不锁"语义正确"**——`g03_call` 以错编译的产物
  入了两个大版本的基线而无人察觉；凡"能编译但入口/语义可疑"的语料须另有运行值钉兜底。

---

## [Unreleased] - 测试基建：外部审计锁与产物基线（PR #155/#156）

**Added**

* **FAST 层 `audit`**（`make test-audit`）：四轮外部审计交付固化为常驻机器锁——
  cc500 E1-E18 一致性快照 + F-01 行为硬断言×3 / minicc 守卫矩阵 13 钉（arity、八进制、
  空/贪心 hex、char 限幅、NUL、递归深度、MC-08 重定义+名字冲突三形、守卫文本 census）/
  cc500 启动不动点 P1==P2（纯宿主 freestanding 回环）
* **FAST 层 `golden`**（`make test-golden` / `make golden-update`）：双编译器对 7 例
  共享语料的产物 sha256 清单入库；重构批产物面验收闸，变更走"声明制"
* **guard-lint**（`tests/guard_diff.sh`，PR/main 直推双覆盖，advisory 起步）：diff 中
  净消失的守卫身份串（`MC-/CC-/F-.../fail("...")`）须以独占行
  `GUARD-CHANGE: all|<消息>` 声明（改钉协议可执行化）

**Engineering**

* 改钉协议四件套成为常规动作：E 快照翻正 + 行为硬钉 + golden-update + PR 声明；
  guard-lint advisory 两周零误报后转阻断（升级路径已注释于 layers.yml）

---

## [v1.5] - 2026-09-11 · 外部审计 A1 分层整改 + NMI 看门狗 + 编译器修复集

> 09-08~09-11 合并 PR #139~#147。主线：外部审计 A1「总体架构与分层」整改（自检/演示/
> 服务彻底移出内核启动路径，DHCP 续约状态机全用户态）+ NMI 看门狗 + cc500/minicc 编译器
> 修复。内核版本串自 v0.33 正式并入 v1.x 线（banner = v1.5，与 changelog 唯一事实源对齐）。

**Engineering**（外部审计 A1 分层整改，PR #144/#145/#146/#147）

* **自检三连移出启动路径（R1.2）**（`SYS_NETDIAG`#38 + shell `netdiag` 命令）：
  ARP/UDP/ICMP 自检不再由内核启动序列硬编码执行，改按需触发；网关 ARP 学习独立为
  功能性路径 `e1000_arp_learn_gw`（外发 IPv4 帧寻址，与自检解耦）
* **demo/服务进 init 脚本（R1.2 后半）**：shell 新增 `bg <prog>`（后台 spawn 不等待）；
  initramfs 写入 `/init.rc`，开机自动 `source`——sockdemo/dhcpd 由用户态按需拉起，
  内核启动序列不再硬编码 spawn 演示进程
* **DHCP 续约迁出中断上下文（R1.3）**：续约执行上下文从 timer ISR 迁至 dhcpd
  守护进程（每 10ms 触发），消除"续约策略在中断上下文运行"的寄生
* **续约状态机全用户态（R1.3 后半）**：内核删除 `e1000_dhcp_tick` 状态机，只保留
  原子能力 syscall#40-44（QUERY/SEND/RECV/APPLY/FALLBACK）；RFC 2131 §4.4.5 的
  T1 RENEW→T2 REBIND→超时重新获取由用户态 dhcpclient 进程决策——机制/策略彻底分离
* **系统调用 ABI 单源化 + 版本化（R1.1，外部审计 A5 整改）**：新建 `src/syscall_table.h`
  （X-Macro 唯一事实源：号/名/默认掩码 + `SYSCALL_ABI_VERSION`=1），user_lib.h /
  userprog.c（enum 展开）与 usermode.c 分发表（枚举 case 标签 + `syscall_tab` 名表 +
  新 `SYS_ABI_VERSION`#45）全部从表派生，消除号表三处手工同步漂移；`sys_kern_audit`
  上报 ABI 版本并自检号表 0..45 连续密集。号一经发布不复用（39 撤销号保留空洞）
* **sys_print 解除 256B 截断上限（R1.1 续，A5 减分项）**：`SYS_PRINT` 由单次
  copyin_str 进 256B 缓冲（>255B 静默截断）改为分块输出直到 NUL——长串完整输出、
  无内建长度封顶（USER_SPACE_END 收口），每块仍逐页校验用户指针；bigdemo 增
  >256B 长打印回归（`LONGPRINT_TAIL` 位于第 260 字节）。`SYS_READLINE` 阻塞
  耦合以契约注释收口：输入已由挂起行队列 + 多等待者循环派发解耦，阻塞为既定语义
* **minicc `\x` 转义贪心 hex + char 差分限幅纪律落码（MC-08#4/#5 整改）**：
  `decode_escape` 的 `\x` 由固定 2 位改为**贪心吃全部十六进制位**并按 char 宽度
  `& 0xFF` 截断（`"\x41F"` 从 'A','F' 两字符修正为单一 0x1F，与 C/gcc 参考一致；
  >28bit 宁拒不坑），minicc.c 与 minicc_self.c 双源同步（自举不动点约束）；
  test_minicc 增宿主 `t_xesc` 编译断言 + guest 运行语义断言（`*s==31`）。
  char 限幅纪律（§9.4 已有文档）补落到 gen.c 代码侧：E_CIDX/E_STR 的 `*hi=255`
  仅为推导上界，运行时值域由初值 rndi(0,127)/ASCII 限定，防后续新增 char 源
  引入 gcc signed char 符号扩展假差异
* **minicc 函数重定义/撞名静默接受收口（MC-08 末角整改）**：根因是重定义判定
  `Sym.val >= 0` 依赖 codegen 期（gen_func）才赋非负的代码偏移，parse 期恒 -1 →
  `int f(){} int f(){}` 被静默放行、调用点 patch 绑定到最后定义（语义漂移且无告警）。
  Sym 增 `defined` 标记（parse 期即可判定：隐式声明=0 / 定义=1），重定义判定改为
  `kind != K_FUNC || defined`，调用点"已定义"判定（原 `val>=0` 同样恒假）一并收口；
  minicc_self.c 并行数组 `sdef[256]` 同构同步（自举不动点 P1==P2 约束）。test_minicc
  增宿主断言：函数重定义/函数撞全局变量/全局变量撞函数三例必须 FAIL + `redefined`，
  调用先于定义（隐式声明补写返回类型）不误伤仍 compiled OK
* **minicc_self 编译器 V3b 特性同步（双编译器差分一致）**：V3b 的 5 个特性（复合赋值
  `+= -= *= /= %=`、前缀/后缀 `++ --`、`do-while`、`break`/`continue`）此前为 host minicc.c
  独有，self 词法/解析/生成三层齐缺（"同一语言两个编译器两套特性"）。现按 §7.3 契约
  "对外语言子集完全对齐"同步补全：词法新增两字符运算符、解析新增 `ND_POST_INC/DEC/DO/
  BREAK/CONTINUE` 节点与前/后缀/复合赋值语法糖、生成器补 `loop_brk/loop_cont` 循环帧栈
  （minicc 不支持多维数组，[32 帧×64 槽] 线性展开，host 用 `[32][64]`）。test_minicc 新增
  [2b4]：构建 hostself（gcc 直编 minicc_self.c）对同一 V3b 用例断言编译通过/拒绝与 host
  一致，且两者产物 **sha256 逐字节相同**；自举不动点 P1==P2 复验通过
* **MC-04 FIX-G 宿主 fidx 错位修复（V3b 同步暴露的既有 bug）**：host minicc.c 调用点核对
  `fidx = si<0 ? nsym-1 : si` 在实参解析后重算 `nsym-1`，实参含函数调用（`pre(g(),2)`，
  `g()` 隐式声明使 nsym 增长）时 fidx 指到实参符号而非被调函数 → 误报 `arg count mismatch`。
  minicc_self.c 一直用 `nval[n]`（正确）。修法：host 改用 `n->val` 对齐 self。此前源码无
  "调用先于定义 + 实参含函数调用"模式未触发；V3b 同步引入 `prefix_incdec(unary(), ND_ADD)`
  后暴露，属既有 bug 而非回归。test_minicc 宿主 107 全绿复验

**Fixed**

* **BUG-074 后续防线（PR #139）**：NMI 看门狗（`wdt.c`）——cli 段死循环可侦测并救回
* **cc500 M8（PR #140）**：前缀 `++`/`--` 栈记账错位——值弹栈改走 `binary2` 配对记账
* **minicc MC-04/06/07（PR #141）**：恢复误删的守卫及对应测试（c6c9272 回归）
* **cc500 M12（PR #142）**：重复 case 常量拒绝——复用 `error()` 报错路径（外部审计）

**Changed**

* **版本串 v0.33 → v1.5**（`src/version.h`，PR #148 文档整改）：banner/motd/回归断言
  统一到 v1.x 线，与 changelog 对齐；`v2-c-kernel/README.md`、顶层 README、design/
  roadmap 相应同步
* **style(usermode)（PR #143）**：还原 SYSLOG 撤销残留的缩进错乱（纯风格）

**cc500 教学里程碑归档（M5~M12，2026-09-06~09-07 落地，随 v1.5 一并入账）**

> 此前 changelog 只记到 v1.4.1·cc500-M5；M6~M12 在 09-06~09-07 直接落地 main，未单独开
> 版本条目，本段补齐（能力快照/里程碑表见 `tools/cc500/README.md`，已同步）。

* **M6（09-06）**：短路逻辑 `&&` / `||`——表达式内跳转实现短路求值
* **M7（09-06）**：三目 `?:`——两分支值路径 + 收尾统一回填
* **M8（09-06；前缀记账修复 09-08 PR #140）**：前/后缀 `++` / `--`
* **M9（09-07）**：`0x…` 十六进制字面量
* **M9b（09-07）**：复合赋值补齐 `/= <<= >>= &= |= ^=`（M4 之外全量）
* **M11（09-07）**：`goto` / 源标签（可前向挂起/后向直回填）
* **M12（09-07；重复 case 拒绝 PR #142）**：`switch` / `case` / `default`（帧栈 + case 表）

**验证**：`make test` 全链绿；test-net 断言零改动即覆盖 init.rc/dhcpclient 新路径；
test-det/tr/rp 确定性套件不受影响。

## [v1.4·cc500-M4] - 2026-09-06 · cc500 复合赋值（+= -= *= %=）+ 入口重定位修复

> cc500（GPL，教学对照自举工具链）新增四则复合赋值 `+= -= *= %=`（复用 `binary2` 算术发射 +
> `'='` 的 lvalue 地址保持路径：`be_push` 驻留地址 → 载旧值 → 求值 → 8/32 位 store）。`/=` 刻意
> 排除（`/` 走注释分支，按 `/` `=` 解析干净报错，纪律#2 不静默）。非 lvalue 目标 → `error()`。
> 顶层全局在首函数前不再打爆入口。

**Changed**（`tools/cc500/cc500.c`）

* **M4**：`get_token` 合成 `+= -= *= %=`；`expression()` 新增对应 else-if 分支调 `compound_assign`；
  `compound_assign()` 单次求值 lvalue 地址（下标内函数副作用恰一次）。
* **Fix（入口重定位）**：原 `be_start` 按「stub 后即首函数」算入口 `call` rel32；顶层全局
  （如 `int g;`）会在 stub 后先 emit 存储，使首函数被推迟 → 入口跳到全局存储、运行即挂
  （PR#115 CI 红根因）。修复：`program()` 内首个函数体处把入口 `call` 重定位到该函数
  （`entry_call_done` 守卫），先全局后函数者入口不再错乱；先函数者零改动。

**Changed**（Tests，`tests/test_cc500.sh`）

* guest `tcalf` 用例改为主函数在前（cc500 契约「入口=源码第一个函数」）、`char *s; s[0]+=1`
  走 compound_assign 8-bit store 路径 + 读回全局 `g==0`，验证入口不被全局存储偏移 + char 下标
  复合赋值运行语义；源码 <128B 走 `writefile` 单行，避开 heredoc。

**验证**：`make test-cc500` 宿主 PASS=28 + guest 全绿 + `ccboot` 自举不动点 PASS（入口重定位
对 cc500 自编译逐字节零影响）。

### F-4：空输入/无入口源干净报错（修复 SIGSEGV）

- 原缺陷：全局 `token`（`char *token`）惰性分配——首次 `takechar()` 才经 `my_realloc` 分配。
  对空/纯空白源一次 takechar 都不触发，`get_token()` 末尾 `token[i]=0` 写 NULL → SIGSEGV
  （host rc=139 而非报错）。
- 修复（`cc500.c`）：`main1` 先 `token = malloc(32)` 预分配，杜绝 NULL 写；`program()` 后若
  `entry_call_done == 0`（无任何函数定义 = 无入口，含空/纯空白/仅注释源）→ 干净报错
  `cc500: undefined symbol`（rc=1），不再编出无入口的废 ELF。
- 测试锚点：宿主新增 `t_empty` / `t_comment_nofn`（期望 FAIL + undefined symbol + 非 compiled OK）。

## [v1.4.1·cc500-M5] - 2026-09-06 · cc500 循环控制（break / continue）补齐

> cc500 拉开 for/do-while 后唯一缺的循环控制面 `break` / `continue`。无 AST 单遍发射下，
> 用"循环帧栈"（进入每层循环压帧记录 break/continue 挂起水位）管理未决跳转：body 内
> break/continue 各 emit `jmp` 并记位置，循环退出 / continue 目标点确定后统一回填 rel32。
> 语义对齐 minicc/C：break=最内层循环出口；continue 目标 while=cond 顶、do=cond 求值起点、
> for=step 起点。支持任意嵌套与同层多次 break/continue。
>
> 工程约束：cc500 无数组声明（`int a[N]` 会被拒）、无类型转换，故帧栈与挂起池一律存于
> 堆（`malloc` 出的 `char*`，4 字节手工打包 `bput4`/`bget4`，与 `save_int`/`load_ptr` 同源）——
> 保证新增源码仍是 cc500 可自编子集，`P1==P2` 自举不动点不受影响。

**Changed**（`tools/cc500/cc500.c`）

* **循环帧栈辅助**：`loop_push / stmt_break / stmt_continue / loop_patch_break / loop_patch_continue / loop_pop`；
  `loop_frames / loop_breaks / loop_conts` 三个堆缓冲（`main1` 预 `malloc`，容量 18 帧 + 两侧各 256 项，
  越界由 stmt_break/continue 计数守卫兜底）。
* **`statement()`**：新增 `break ;` / `continue ;` 分支（`loop_depth<=0` 时 `error()`——循环外不得静默）；
  `while` 分支改穿帧（push→body→回填 break→exit、continue→cond 顶→pop）。
* **`stmt_for()`**：body 穿帧，continue→`p_step`（step 起点）、break→退出点。
* **`stmt_do()`**：body 穿帧，continue→cond 求值起点（`pc`）、break→`p2`（构造后退出点）。

**Changed**（Tests，`tests/test_cc500.sh`）

* 宿主新增 `t_brk`/`t_cnt`/`t_dobc`（编译 OK）+ `t_brk_oob`/`t_cnt_oob`（循环外 break/continue 须 error
  rc=1 非 compiled OK）。
* guest 新增 4 个运行语义（for-break 求和 10 / for-continue 偶数求和 20 / do-break 求和 10 / 嵌套
  continue s==4）。源码较长走 heredoc（`writefile <<M`）规避 128B 单行截断，并在每次 ccrun 后 `rm`
  释放 guest FS inode（此前 4 用例连建的 .c/.elf 撑爆 mini-fs inode，`/tnest.c` create 返 -1）。

**验证**：`make test-cc500` guest 全绿（含 M5 4 项）+ `ccboot` 自举不动点 PASS（break/continue 帧栈
对 cc500 自编译逐字节零影响）。

## [v1.4/minicc] - 2026-09-06 · minicc 循环控制补齐（do-while / break / continue）

> minicc（自研 MIT 编译器）循环控制按 C 习惯补齐：新增 `do`-while 后测循环 + 循环内
> `break`（提前退出）与 `continue`（跳到下一次迭代）。基于 AST 的单遍 codegen 用"循环帧栈"
> 记录未决跳转位置，循环收尾统一回填相对位移——支持嵌套与同层多次 break/continue。
> 语义与 C 一致：break 跳最内层循环出口、continue 跳到该循环"下次迭代入口"（while/for 为回测
> 条件或执行 step，do-while 为回测 body 后的条件）。

**Changed**（`tools/minicc/minicc.c`）

* **Node 枚举**：新增 `ND_DO` / `ND_BREAK` / `ND_CONTINUE`。
* **CC 上下文**：新增循环帧栈 `loop_brk/loop_cont/nloop`（未决跳转位置，规避 labs[] 单补丁限制）。
* **`stmt()`**：新增 `do <body> while(<expr>);`、`break;`、`continue;` 解析。
* **`gen_stmt`**：`ND_DO`（post-test，`jnz` 直接回 body）、`ND_BREAK`/`ND_CONTINUE`（帧内记录 +
  收尾回填）；`ND_WHILE`/`ND_FOR` 接入循环帧（continue 目标 for 指向 step、while 指向条件）。
* **循环外 break/continue → 编译期报错** `break/continue outside loop`。

**Added**（Tests，`tests/test_minicc.sh`）

* 宿主错误路径：`break`/`continue` 循环外 → FAIL。
* 宿主成功路径：do / break / continue / do+break 编译层 PASS。
* guest 运行语义（heredoc 多行源）：do-while 累 0..9==45、for+break 累 0..4==10、
  continue 跳偶累奇==25、嵌套 break 只断内层 s==3，均 `code=0 PASS`。

**Added**（Tests，`tests/test_minicc_mock.c` 白盒）

* 新增 `t_stmt_do` / `t_stmt_break` / `t_stmt_continue`：断言 do→`ND_DO`（body/cond）、
  break→`ND_BREAK`、continue→`ND_CONTINUE` 的 AST 形状。

**Added**（差分对拍，`tools/minicc/diffsynth/gen.c`）

* 新特性入网：`F_DO/F_BRK/F_CNT`（仅 `CAPS_MINIC`，cc500 保守基座不加）；
  `stmt_gen` 新增 do-while + break/continue 语句模板（`_d<20` 限幅保终止），
  使差分/自覆盖探针覆盖 do/break/continue（gcc↔minicc 差分 + 确定性）。

**Engineering / Docs**

* `docs/design/minicc-design.md` §6.1 Feature Matrix：语句支持加 `for`/`do`/`break`/`continue`，拒绝列表摘除。
* `changelog.md`：本条目。

## [v1.4·minicc-sugar] - 2026-09-06 · minicc 复合赋值（+= -= *= /= %=）与前/后缀 ++/--（纯语法糖）

> **纯语法糖，无新 emit 原语**：`lv op= rhs` 在 parser 层改写成 `lv = (lv op rhs)`（`ND_ASSIGN` 套
> 二元 `ND_ADD/SUB/MUL/DIV/MOD`），前缀 `++lv/--lv` 改写成 `lv = lv±1`；后缀 `lv++/lv--` 因 C 语义
> **值为旧值**而引入单节点 `ND_POST_INC/ND_POST_DEC`，其 codegen 仅复用现有 load/store 指令（ebx 暂存
> 左值地址，读旧值→±1→写回→弹回旧值），**不新增任何 emit 字节原语**。词法新增两字符运算符
> `+= -= *= /= %= ++ --`。
> - 契约：非左值复合赋值/自增自减 → 编译期静态报错（`assign to non-lvalue` /
>   `increment/decrement of non-lvalue`），绝不出坏码。
> - 差分对拍（diffsynth）新增 `F_SUGAR` 入网：安全子集 `+=(2..9) -= (2..9) /= %%=(2..9) ++ --`；
>   刻意排除 `*=`（重复累乘会令值域逃逸有符号溢出，与"无 UB 三纪律"冲突）。
> - 三层验证（三同步）：host（test_minicc.sh 编译层）、Mock 白盒（AST 形状 + 全管线 codegen，断言
>   76→96）、guest（test_minicc.sh 运行语义：前缀新值/后缀旧值/复合链/指针 p+=1）。

## [v1.4·netif] - 2026-09-06 · 虚拟 TCP 下行滑动窗口（host→guest 停-等→滑动窗口，吞吐 W/RTT）

> 下行可靠由 v1.2 停-等升级为**滑动窗口**（与 v1.3 上行镜像）：转发器发送窗口 `DWIN=8`（=guest
> `TCP_RXWIN`）×独立下行 seq，最多 8 报同时在途；guest 接收侧为滑动窗口接收端（seq 落窗口内即缓存
> 进 `rx_win` 重排缓冲，`rx_flush` 凑齐连续后依序送 `rxb`、累计推进 `rx_next`），回累计 ACK（下一
> 期望下行 seq）推进转发器 `dl_base` 腾窗续发；最老未确认槽超时重传。`MSG_ACK` 累计语义复用、
> **不新增消息类型、不改协议头结构**。吞吐从"1/RTT"提至"W/RTT"。

**Changed**（guest 薄包装，`src/app/tcp.h`）

* **新增** **`rx_slot_t`** **结构体**：下行接收侧重排缓冲槽（busy/len/data），`rx_win` 下标 = `seq % TCP_RXWIN`。
* **新增** **`TCP_RXWIN`** **宏**：`#define TCP_RXWIN 8`，下行接收窗口最大允许"乱序暂存"的包数上限（镜像 `TCP_TXWIN`）。
* **`tcp_conn_t`** 新增 `rx_win[TCP_RXWIN]` 字段（重排缓冲）；`rx_next` 语义从"严格期望单值"升级为"最老未交付下界/累计确认边界"。

**Changed**（guest 薄包装，`src/app/tcp.c`）

* **新增** **`rx_flush`** **辅助函数**：从 `rx_next` 起交付"连续已到"的重排缓冲并推进 `rx_next`。
* **`drain`** **MSG\_DATA 处理重写**：从"只收 `seq==rx_next`、否则丢弃"升级为"窗口内缓存 + 凑齐连续交付 + 累计 ACK"；
  窗口外（超窗/已交付区重复）丢弃载荷、仅重发 ACK 自愈。
* **`tcp_open`** 初始化：新增 `for` 循环清空 `rx_win[i].busy`（防上次连接残留）。

**Changed**（宿主转发器，`tests/tcp_proxy.py`）

* **Session 下行状态重构**：移除停-等单槽 `pending/seq/inflight/inflight_seq/inflight_t`，新增
  `dl_pending` / `dl_seq`（递增分配器）/ `dl_base`（累计确认边界）/ `dl_win={}`（在途副本 `{seq:(payload,t)}`）。
* **Proxy 类**：新增 `DWIN = 8` 常量（与 guest `TCP_RXWIN` 对齐）。
* **新增** **`_dl_fill`**：填满窗口（≤DWIN）即发，窗口满停；eof 且数据发尽、窗口空后发 `MSG_CLOSED`（保证不缺尾）。
* **新增** **`_dl_retrans`**：最老未确认下行槽超时重传（SR 风格）。
* **`MSG_ACK`** **处理**：从"单报 ack==inflight+1"升级为累计推进——`advance=(ack-dl_base)` 内在途范围内
  清槽、`dl_base=ack`，随后 `_dl_fill` 决定续发/发 `MSG_CLOSED`。
* **`_tcp_read`/`_sweep`**：引用改 `dl_pending`/`_dl_fill`/`_dl_retrans`。

**Added**（Tests）

* **`tests/test_downlink_window.py`**：宿主侧**下行发送窗口**确定性单测（自包含，拉起 proxy）——首轮在序填满
  DWIN 后满窗不再爆；不发 ACK → 最老未确认槽超时重传（seq0 重复）；累计 ACK 逐窗推进至 MSG_CLOSED；
  全量按序不重不漏 == 上游 BLOB。验证"窗口上限/累计推进/最老槽重传/不缺尾"。

**Engineering / Docs**

* `tcp-session-proto.md`：头部注记 v1.4 + §6 重写（6.1 下行滑动窗口、6.3 候选→✅ 已落地）、附录 A.5 项 5 更新。
* `changelog.md`：本条目。

## [v1.4.17] - 2026-09-05 · OBS 观察工程化（R1/R2/R3）+ 加固 A-2（BUG-073+）

> **OBS 观察工程化**（红队/评估观察入库收尾）：
> - **R1（串口 kb 通道告警，OBS-R1）**：kb 通道对 ≥0x80 单字节静默丢弃（实测非 ASCII 载荷逐字节丢失且无告警）。
>   改 `kb_feed_char` 对非 ASCII 高位字节累计计数，行结束/取行时若计数>0 打印 `[kb] N non-ascii bytes dropped`
>   （纯 ASCII 全链路零新输出）。仍不做 UTF-8 全支持/转义通道（另立项）。
> - **R2（cc500 文法文档对齐，OBS-R2）**：README"用局部数组替代"表述与实现不符（statement() 无 `[` 分支，
>   局部数组同样被拒）。改文档为"数组声明不支持（局部与全局均不支持），对已有缓冲的下标访问 `p[i]` 仍支持"，不扩文法。
> - **R3（fs 固定槽位句柄契约，OBS-R3）**：`sys_fs_open(fd,name,mode)` 的 **fd 是调用方写死的固定槽位号，返回值
>   0=成功 / -1=失败**（红队 D2 曾把返回 0 当"成功句柄"致后续 fs 操作全被 -1 拒的假阳性）。`user_lib.h` 新增
>   `open_at(fd,name,mode)` 包装 + 契约注释；fsdemo 改用之冒烟。
>
> **加固 A-2**：
> - **① DHCP 应答源校验**（红队 C2 enabler，RFC 2131 §4.1）：续约应答不再只按"端口 68 + 可预测 xid"分发。
>   `dhcp_poll_once` 收包处要求 op==BOOTREPLY、UDP 源端口==67、已绑定则源 IP 匹配 `dhcp_server_ip`，否则整包丢弃并打一行日志
>   （SLIRP 源 10.0.2.2:67 天然通过，test-net 零回归）。`netsock_dhcp_recv` 扩展出参暴露源 IP/端口。
> - **② syscall 负参/哨兵语义清理**（BUG-067 延续）：`sys_sem_create` 负 init → 显式拒绝（SEM-1 审计误报根因）；
>   fs_seek 负/巨大偏移按无符号原生存放，越界由 read/write fail-closed（补注释）；brk 上下界原已钳位。全部处理原则汇总为
>   `usermode.c` syscall_dispatch 头部"参数语义表"（① id/槽位双检 ② 指针整区校验 ③ 长度/容量 clamp ④ 有符号语义拒绝 ⑤ 地址/偏移）。
> - **③ docs/reference/security.md**：安全威胁模型显式化（信任边界 / 取舍 / 已知观察对账 / 新件自查清单），接入 docs/README。

**Engineered**

* [src/drv/kb.c](../../v2-c-kernel/src/drv/kb.c)：非 ASCII 丢弃计数 + 行结束/取行上报（OBS-R1）。
* [src/app/user_lib.h](../../v2-c-kernel/src/app/user_lib.h) + [fsdemo.c](../../v2-c-kernel/src/app/fsdemo.c)：`open_at`/`sys_fs_*` 包装 + 契约注释（OBS-R3）。
* [src/net/netsock.c](../../v2-c-kernel/src/net/netsock.c) + [netsock.h](../../v2-c-kernel/src/net/netsock.h)：`netsock_dhcp_recv` 出参暴露源 IP/端口。
* [src/drv/e1000.c](../../v2-c-kernel/src/drv/e1000.c)：`dhcp_poll_once` 加 op/源端口/源 IP 三重校验 + 丢弃日志（A-2 ①）。
* [src/kernel/usermode.c](../../v2-c-kernel/src/kernel/usermode.c)：`sys_sem_create` 负 init 拒绝 + fs_seek 注释 + dispatch 头部"参数语义表"（A-2 ②）。
* [README.md](../../README.md)、[docs/reference/bugs.md](../../docs/reference/bugs.md)、[docs/reference/security.md](../../docs/reference/security.md)、[docs/README.md](../../docs/README.md)：R2/R3 + security.md + BUG/OBS 登记。

**自测实录**

* `make`（-Werror）干净；`make test-host` pass 全绿；`make test-serial` / `make test-net` 全绿。
* 手验：DHCP 续约源校验——SLIRP 应答（10.0.2.2:67）通过、错误源 UDP 注入被拒并出 `[dhcp] drop:` 日志（验证后临时代码已还原）。
* Docs：bugs.md 登记 OBS-R1/R2/R3（已修复）+ BUG-073（加固 A-2）。

## [v1.4.16] - 2026-09-05 · 加固 A-1：内核 SSP + panic 寄存器/调用栈回溯 + ring3 chaos 探针（BUG-072+）

> 承接 #69（异常族单点已清）后的可观测性加固：编译期兜底 + 故障可观测性两层，非单一崩点修复。
> ① 内核 SSP：CFLAGS 开 `-fstack-protector-strong -mstack-protector-guard=global -fno-omit-frame-pointer`；
> `ssp_seed()` 用 RDTSC 随机化 `__stack_chk_guard`（区别 app 层固定值防绕过），`__stack_chk_fail` 打印
> `[PANIC] kernel stack canary mismatch` 后停机——宁要可诊断停机不要静默内存破坏。
> ② panic 现场增强：`tools/gen_kernsym.sh`（`nm` 提取）+ 两阶段链接嵌入符号表，`ksym_name` 二分符号化 +
> `dump_backtrace`（EBP 链）+ `panic_dump`（寄存器 + eip 符号 + 内核态回溯/ring3 注明），接入 idt.c 异常路径。
> ④ ring3 chaos 探针：`run chaos` 每轮 fork 子进程随机执行 ud2/int3/cli/hlt/div0/lgdt 一条，
> 内核隔离杀该子进程（panic_dump 现场 + sched_kill），父进程连跑 6 轮后自审计 0 存活，接入 test-serial 断言。
> 非目标：不引入 PAE/NX、不做 ASLR、不变中断模型、不扩 KSTACK。

**Engineered**

* [Makefile](../../v2-c-kernel/Makefile)：CFLAGS 启用 SSP/帧指针；OBJS 增 `ssp.o` `ksym.o`；两阶段链接
  （`kernel_nosym.elf` → `gen_kernsym.sh` → `ksym_tab.c` 重链），`.DEFAULT_GOAL` 固定最终内核；APPS 增 `chaos`。
* [ssp.c](../../v2-c-kernel/src/kernel/ssp.c)：RDTSC 混合随机化内核栈金丝雀 + `__stack_chk_fail` panic dump。
* [ksym.c](../../v2-c-kernel/src/kernel/ksym.c) + [gen_kernsym.sh](../../v2-c-kernel/tools/gen_kernsym.sh)+ [ksym_stub.c](../../v2-c-kernel/tools/ksym_stub.c)：
  符号表二分 + 寄存器 dump + EBP 回溯；接入 [idt.c](../../v2-c-kernel/src/arch/idt.c)（用户态异常隔离杀进程）。
* [kernel.c](../../v2-c-kernel/src/kernel/kernel.c)：`kernel_main` 早期（serial_init 后）执行 `ssp_seed()`。
* [apps/chaos.c](../../v2-c-kernel/src/app/chaos.c) + [storage.c](../../v2-c-kernel/src/fs/storage.c)：ring3 随机坏指令探针注册入 initramfs。
* [test_serial.sh](../../v2-c-kernel/tests/test_serial.sh)：`run chaos` 断言 survived rounds audit=0 + 无 NOT_TRAPPED
  （`[0-9]+` 需写作 `[0-9][0-9]*`——grep 默认 BRE 中 `+` 是字面量，非量词）。

**自测实录**

* `make`（-Werror 默认开）干净；`make test-host` pass=20 fail=0；`make test-stack` OK；`make test-serial` 全绿（含 chaos 断言）；`make test-qemu` 通过。
* 手动验证：内核态临时栈溢出 → 串口出 `[PANIC] kernel stack canary mismatch (__stack_chk_fail)` 后停机（非静默）；chaos 各轮 #UD/#DE/#GP dump 寄存器 + ring3 隔离，6 轮后系统存活。验证后临时代码已还原。
Docs：bugs.md 登记 BUG-072+（加固 A-1）。

## [v1.4.15] - 2026-09-04 · Red Team 四轮 RD5：块归属账本——关闭"合法范围内重复块"（BUG-071）

> 红队四轮 RD5-V4，属 RD3（BUG-069/070）诚实边界清单中的**内容层矛盾**。RD3 只做块号**范围校验**
> （`blk_valid`），挡不住恶意镜像把两个 inode 的 `blocks[]`/`indirect` 指向**同一合法范围内的数据块**——
> 造成跨文件读泄漏 / 写污染，且若该块归受保护（`FS_MODE_RO`）文件所有，可经可写 inode 写改 RO 内容，
> **击穿 BUG-057**。修复在 fs.c 建立"块 → owner inode"归属账本：新增块（目录扩容/直接/间接/拾取）alloc 后
> `owner_claim` 登记、返回前 `owner_check` 校验（不符即视为不可用，读截断 / 写按首块 -1 语义）；挂载外部镜像
> 后 `fs_scan_owners` 全盘扫描重建账本并检出"范围内重复"（`fs_owner_violations` 仅观测、不入 audit bad 和）；
> 释放即 `owner_clear` 清账。owner 值用 **ino+1 编码**（0=空闲，1..64↔ ino 0..63）避免 root(ino 0) 与空闲歧义。
> 全链零行为变更（正常镜像账本跟随分配/释放天然一致，引导恒 0 冲突 0 孤儿），纯防御 + 可观测。
**Changed**
* [src/fs/fs.c](../../v2-c-kernel/src/fs/fs.c)：`file_block` 增加 `ino` 参数并做块归属登记/校验（直接/间接/拾取块
  各自 `owner_claim` + `owner_check`）；`fs_init` 格式化重置账本；`free_inode_blocks` 释放即清归属；
  `dir_add` 目录自扩块登记。新增 `owner[]` 账本 + `fs_scan_owners` + `fs_owner_orphans`。
* [src/fs/fs.h](../../v2-c-kernel/src/fs/fs.h)：导出 `fs_scan_owners`/`fs_owner_violations_get`/`fs_owner_reset`/
  `fs_owner_orphans`。
* [src/fs/storage.c](../../v2-c-kernel/src/fs/storage.c)：持久盘命中 `FS_MAGIC` 挂载即 `fs_scan_owners` 重建账本。
* [src/kernel/usermode.c](../../v2-c-kernel/src/kernel/usermode.c)：`kern_audit` 打印块归属冲突次数（仅观测）。
**Tests**
* [test_fs.c](../../v2-c-kernel/tests/test_fs.c)：新增 V4 四式——① 健康基线（scan 后 0 冲突 / 0 孤儿，无假阳性）；
  ② 跨文件重复（B 指向 A 合法数据块 → read 截断 / write -1，A 不受影响）；③ V4×RO（可写 W 指向受保护 P 块 →
  写被首块阻断、P 内容与只读位不变）；④ 间接块重复（Y 的 indirect 指向 X 间接块 → scan 检出、越直接区读写阻断）。
**自测实录**
* `make test-host`（-Werror 默认开）：pass=20 fail=0（test_fs 8785 checks 0 fail）。
* `make`（内核 m32 -Werror）：干净零告警。
Docs：bugs.md 新增 BUG-071 + 威胁模型注记扩展（RD3 诚实边界 → RD5-V4 已收口 + RD5 新边界声明）。

## [v1.4.14] - 2026-09-04 · 门禁 flaky 治理：长名断言时序根修 + 等断观测打点（产物随 artifact）+ RR 基线
> 承接插曲 1/2 + 治理任务 C（三步）。Commit 1（行为变更，ops 已批准）：test_serial.sh 长名用例
> 两处 `run=[1-9]` 在 10ms tick 边界下把 run=0ms 的合法快执行误判 FAIL（类型 II 断言缺陷）——收敛为
> `run=[0-9]+`，真实性交由 `[elf] loaded` + 程序自打印承担，BUG-066 防回归由 F2 负向断言独立覆盖。
> Commit 2（纯观测）：三脚本 wait_for/wait_after 每断言恰打一行
> `$BUILD/assert_timing_<script>.tsv`（断言名\t耗时ms\tok|timeout，断言名 sed 转义防列错位，
> `make clean` stash→放回保台账），FAIL/超时分支打印 LOG 尾 ~20 行现场（区分类型 I 整行缺=时序 vs
> 类型 II 行在格式不符=断言）。Commit 3（纯观测）：tr2sqlite.py 新增 assert_timing 表（幂等导入）、
> baseline_check.py 新增 --asserts 跨轮 P50/P95 + 变慢预警。Commit 4（纯文档）：bugs.md 观察表
> OBS-013、README 维护规则、本文档。全链不新增 CI job、不改 job 名、不动 B2 20s 阈值。
**Tests**
* [test_serial.sh](../../v2-c-kernel/tests/test_serial.sh)：长名断言 `run=[1-9]`→`run=[0-9]+`；
  新增 wait_for 打点 + FAIL 现场 dump（`$BUILD/assert_timing_serial.tsv`）。
* [test_socket.sh](../../v2-c-kernel/tests/test_socket.sh) / [qemu_regression.sh](../../v2-c-kernel/tests/qemu_regression.sh)：
  同构打点（`_socket.tsv`/`_qemu.tsv`），分文件防 `make test` 串行互覆盖。
* [Makefile](../../v2-c-kernel/Makefile)：`clean` 对 `assert_timing_*.tsv` stash→放回，保三者并存。
* [tr2sqlite.py](../../v2-c-kernel/tests/tr2sqlite.py)：新增 assert_timing 表 + `--assert-timing` 导入（幂等）。
* [baseline_check.py](../../v2-c-kernel/tests/baseline_check.py)：`--asserts` 对 (script,assert) 跨轮 P50/P95 + 变慢预警。
**自测实录**
* 三脚本串行 `make test` 后 `assert_timing_{serial,socket,qemu}.tsv` 并存（serial=94/socket=14/qemu=97 行）。
* `make`（-Werror 默认开）干净；`make test-host` pass=20 fail=0。
* tr2sqlite 对同 runid 重复导入幂等（total 不翻倍）；`baseline_check --asserts` 输出 P50/P95/timeout/变慢预警。
Docs：bugs.md 观察表追加 OBS-013（门禁 flake 台账，类型 I/II 治理口径）；docs/README.md 追加规则 9（观测产物/断言名稳定性/GNU date/MD047）。

## [v1.4.13] - 2026-09-04 · Red Team 三轮 RD3：fs 读路径块号信任——块号损坏统一拒堵（BUG-069/070）

> 红队三轮 RD3 两项修复（共享同一根因 `blk_valid` 仅用于释放路径）：恶意镜像把磁盘 inode 的
> `blocks[]`/`indirect` 当块号直接寻址且读/写/遍历不校验，造成两类链路——**V2（高）** 块号指向 inode
> 表（如 `blocks[0]=3`）→ 文件读写别名作用于 inode 表，清掉受保护文件的 `FS_MODE_RO`，**击穿 BUG-057
> 系统文件只读**（`fs_is_ro 1→0`、内容被改、静默无崩）；**V1（中）** 块号越界 → `blockdev_ptr` 返 NULL
> 被解引用（`fs_lookup_in`/`file_block`，宿主 ASan SEGV；guest 低 16MB 恒等映射使 NULL 写不触发 #PF →
> 静默破坏）。修复将 `blk_valid` 前置到一切 inode 块号寻址路径（目录增删空查列视同损坏跳过；`file_block`
> 统一"损坏块号==0"：直接/间接/拾取块号损坏返回 0，读截断、写依既有首块 -1/短写），并加判空纵深。
> fs 层纯逻辑，宿主单测直接复现 V1/V2、断言读写拒绝、只读不击穿、ASan 清洁 + 正常性对照。诚实边界
> 注记 bugs.md：本修复断"元数据区被当数据读写"；两文件 inode 直指同一合法数据块这类内容层矛盾需镜像
> 校验工具承担，不属本修复范围。

**Fixed**

* [fs.c](../../v2-c-kernel/src/fs/fs.c)：`blk_valid`（`blk∈[FS_DATA_START, bd->blocks)`）推广到一切 inode 块号
  寻址路径，区块 0-3（位图/inode 表）永不作为数据块寻址：
  * `dir_add` / `dir_remove` / `dir_empty` / `fs_lookup_in` / `fs_list_dir`：`if (!di.blocks[b])` 后加
    `if (!blk_valid(bd, di.blocks[b])) continue;`——损坏块视同跳过，不解引用、不参与找槽（V1 防崩 + V2 防别名）。
  * `file_block`：直接块非 0 但 `!blk_valid` → return 0；indirect 非 0 但 `!blk_valid` → return 0；
    拾取块号非 0 但 `!blk_valid` → return 0；`create=1` 时损坏块号不可重分配覆盖（可能被镜像别处引用），
    返回 0 走 `fs_write` 既有"首块失败 -1 / 中间块短写"、`fs_read` 既有 `blk==0 break` 截断。
  * 判空纵深：上述 5 处 `blockdev_ptr` 返回后、解引用 `e` / `ptrs` 前加 `if (!e) …`（正常不可达，纯纵深）。
  * `blk_valid` 定义处注释由"SEC-03 释放路径"更新为"RD3：一切 inode 块号寻址路径统一前置校验"。

**Tests**

* [test_fs.c](../../v2-c-kernel/tests/test_fs.c)（宿主，并入 `make test-host`，ASan 清洁）：
  * V2（BUG-069）：建 A + protect P → 篡改 blockdev inode 表令 A.blocks[0]=3（inode 表）→ `fs_read/write(A)`
    拒绝（-1/0），P 仍含 `FS_MODE_RO`、内容未变。
  * V1a（文件直接块=0xFFFFFFF0）：read 截断为 0、write 首块 -1、不崩；V1b（indirect=越界）：间接区 read
    截断、write -1、不崩；V1c（目录块=越界）：`fs_lookup_in`/`fs_list_dir` 安全返回、不崩。
  * 正常性对照：恢复合法块号后读写/遍历结果不变（校验不误伤）。

**自测实录**

* `make` -Werror 干净；`make test-host` pass=20 fail=0（含新增 RD3 断言）；`make test-qemu` 全绿。

Docs：bugs.md 新增 BUG-069（RD3-V2）/ BUG-070（RD3-V1），附威胁模型注记（诚实边界：内容层一致性交镜像校验工具）。

## [v1.4.12] - 2026-09-04 · Red Team 二轮：readline max=0 早退 + 悬垂 fd 回收（BUG-067/068）+ 观察记录入账

> 红队二轮两项修复 + 四条观察入账：① `sys_readline(max=0)` 旧实现把 0 当"未指定"替换成
> KB_LINE_MAX+1(129)，向零容量调用方缓冲写整行（契约违反）——max=0 直接显式失败返 -1；② 删除文件后
> 仍打开的 fd 持 inode 号，inode 最低位被新文件复用时旧 fd 写会静默落入新文件（跨文件写）——删除成功
> 即经调度层 `sched_fd_revoke` 回收指向该 inode 的悬垂 fd。观察记录（不改码、仅入账）：OBS-R1（串口 kb
> 通道丢 ≥0x80 字节）、OBS-R2（cc500 无数组文法 vs README"用局部数组"表述）、A4（存活父进程僵尸钉死
> pid 槽=设计权衡）、A5（sleep 巨值回绕=立即唤醒）。

**Fixed**

* [usermode.c](../../v2-c-kernel/src/kernel/usermode.c)：`case 20 sys_readline` 在 `user_ptr_valid`
  之前加 `if (b == 0) { r->eax = (uint32_t)-1; return; }`——max=0 不是"未指定"哨兵，调用方必须给真实
  容量，0 直接显式失败以尽早暴露非法调用方（防向零容量缓冲静默写整行，RBT-2026-013，BUG-067）。
* [sched.c](../../v2-c-kernel/src/kernel/sched.c) 新增 `sched_fd_revoke(uint32_t inode)`：遍历
  `procs[1..MAX_PROCS)`，对非 FREE 进程 fd_table 中 `used && fd.inode==inode` 的槽置 used=0，记日志
  `[fs] revoke fd=%u inode=%u (pid=%u)`；相应 [sched.h](../../v2-c-kernel/src/kernel/sched.h) 声明。
* [usermode.c](../../v2-c-kernel/src/kernel/usermode.c)：`case 19 fs_delete` / `case 28 fs_rmdir` 在 fs
  返回成功（0）后以解析得到的实际 inode 调用 `sched_fd_revoke(ino)`——防 inode 复用后被旧 fd 写落到
  新文件（跨文件写、无告警，RBT-2026-014，BUG-068）。fs 层（纯逻辑、宿主单测直接编译）零改动。

**Tests**

* [test_serial.sh](../../v2-c-kernel/tests/test_serial.sh)：新增双向回归（载荷 cc500 方言合规，
  `0-1` 规避 unary 负号、无数组/for/break/cast；断言按 SN 行号切片取 ccrun 运行段排除回显假阳性）——
  G1：`sys_readline(合法指针, 0)` 返 -1 且不阻塞；D4：create A→open fd1(A)→write→rm A→create B→open
  fd2(B)→write BOK→经悬垂 fd1 写：断言 `[fs] revoke` 日志、fd1 写返 -1、`cat /dB` 仅含 BOK 绝无旧 fd 字节。

**自测实录**

* `make` -Werror 干净；`make test-host` pass=20 fail=0。
* `make test-serial` / `make test-qemu` 全绿（含 G1/D4 新断言）。

Docs：bugs.md 新增 BUG-067/068，观察记录表追加 OBS-R1/OBS-R2/A4/A5。

## [v1.4.11] - 2026-09-04 · Red Team F1/F2：按名/按路径加载缓冲契约对齐 + ccrun 假成功判败（BUG-065/066）

> 红队审计两项收口：① 内核按名/按路径加载的 16B 缓冲与 FS 契约（FS_MAX_NAME=24）不一致，
> 16~23 字符合法程序名被静默截断撞前名前缀、可能误加载错误程序——缓冲统一 64B（path 约定）
> 并引入 `copyin_str_full` 对超长**显式失败**不再静默截断；② shell `cmd_ccrun` 用 uint32_t 收
> `sys_spawn_file` 失败返回值，-1 化 4294967295 使判败恒假、把"根本没运行"伪装成假 `PASS (run=0ms)`——
> 改有符号判败并显式报 `cannot run`。

**Fixed**

* [usermode.c](../../v2-c-kernel/src/kernel/usermode.c)：三处按名/按路径加载缓冲
  `char namebuf[16]` → `char namebuf[64]`（`usermode_spawn_elf` name 缓冲、`sys_exec_case` namebuf、
  `case 21 sys_spawn_file` namebuf），与 `case 13/14/18` 的 `char path[64]` 及 FS 路径契约一致；
  `sys_exec_case`/`case 21` 改用 `copyin_str_full`，超长显式返回 -1（日志
  `spawn_file name too long/invalid`），不再静默截断撞前缀加载错误程序。
* [userptr.c](../../v2-c-kernel/src/kernel/userptr.c)：新增 `copyin_str_full`——同 `copyin_str`
  但不静默截断，max 内未遇 NUL 返回 -2（超长），dest 已安全终止；供"按名/按路径加载"边界使用。
* [sched.h](../../v2-c-kernel/src/kernel/sched.h)：进程名显示缓冲 `pcb.name_buf` 16→64B，
  与加载缓冲契约一致化，消除进程名显示被早截断的隐性契约。
* [shell.c](../../v2-c-kernel/src/app/shell.c)：`cmd_ccrun` 运行段改 `int spid` 接收
  `sys_spawn_file` 返回值，`spid<=0` 正确判败并打印 `[ccrun] cannot run '<out>'`，消除假 PASS。

**Tests**

* [test_userptr.c](../../v2-c-kernel/tests/test_userptr.c)：mmap 出"假页表已映射低区"后，
  真读覆盖 `copyin_str_full` 三态（成功 0 / 无效或未映射 -1 / 超长 -2），36 断言全绿。
* [test_serial.sh](../../v2-c-kernel/tests/test_serial.sh)：新增双向回归——正向"20 字符源名 +
  18 字符加载名"真实加载运行（`[elf] '...' loaded` + 运行输出 + `PASS (run>0)`）；负向"不存在的源"
  必须显式 `compile FAIL`，且命令窗口切片内不得再出现 `exited code=0 PASS`（防 run=0ms 假阳性归来）。

**自测实录**

* `make` -Werror 干净；`make test-host` 全绿（test_userptr 36 断言）。
* `make test-serial` 通过（长名加载运行 PASS + 失败显式无假 PASS 两向断言通过）。

Docs：bugs.md 新增 BUG-065/066。

## [v1.4.10] - 2026-09-03 · ELF 段尺寸钳制 + mapfn 失败中止 + heap 页整倍缺陷（BUG-056）

> DoS 审计落地：堵"单条 guest 命令整机宕机"的第 5 类向量（ELF 段 p_memsz 无上限叠加 mm 低
> 16MB 恒等映射假设）。把加载区间钳到用户半区 ≤1MB、`elf_load` 在映射失败时中止（不再清
> bss 未映射区）、`kmalloc` 补页计数计入块头（修"可用内存却 OOM"）。全部返回 -1 式降级，
> 未开新 cli;hlt、未改 guard.c。

**Fixed**

* `load_elf_file`（usermode.c）：`elf_load_range` 后强制 `lbase∈用户半区 && lend≤END && 区间≤1MB`
  否则 -1 —— 畸形 p\_memsz 在映射前钳死。
* `elf.c`：`elf_map_fn` 改返回 int；`mapfn` 返回非 0 时 `elf_load` 立即中止，不再 memcpy/memset
  未映射区（规避"部分映射后清 bss 触碰空洞/高位页表帧"的内核态缺页路径）。
* `app_mapfn` 恒返回 0/非 0（失败即向 `elf_load` 广传）。
* `heap.c`：`kmalloc` 补页 `need` 计入 `HDR_SIZE`——修"请求恰为 N×4096 时新增块恒差 16B 而
  分配失败"的功能缺陷（审计 P1）。

**Tests**

* DoS 夹具 `zbig`（84B 畸形 ELF，p\_memsz=96MB）入 initramfs；qemu\_regression / test\_serial
  `run zbig` 门禁断言 `cannot load 'zbig'`（必 -1）、整机不 [FATAL]。
* test\_elf 新增 4.11：mapfn 映射失败时 `elf_load` 中止且不写目标区/bss。

**自测实录**

* `make test-host` pass=20 fail=0（test\_elf 39 断言）。
* `make test-qemu` 全量通过（bigdemo 70KB / cc500 自举 / shell 等合法加载不误伤；`run zbig` 门禁过）。
* `make test-serial` 通过（`zbig 被 -1 拒绝`）。

Docs：bugs.md 新增 BUG-056；残余 P2（>16MB 元数据帧专用池）另行立项。

## [v1.4.9] - 2026-09-03 · pf_handler 降级：内核态命中用户半区缺页改杀进程、不再停整机（BUG-055）

> 纵深防御第四道防线收口：`user_ptr_valid`（第一层入口）旁路之后，pf_handler（第四层审计）过去
> 仍以 `cli;hlt` 整机停机兜底。把"内核态访问用户半区未映射页"从 `[FATAL] 停机`降级为
> `sched_kill(current) 杀进程`，兑现"最坏只能是杀进程"的铁律（任何单一用户程序的错误绝不
> 触发整机宕机）。已实测：模拟 syscall 绕过收敛点时，缺页由 `[FATAL] 停机` 变为 `[kern] kill`，
> 整机心跳持续。

**Fixed**

* `src/mm/mem.c` 的 `pf_handler`：CPL=0 且 fault ∈ 用户半区 `[USER_SPACE_BASE, USER_SPACE_END)` 时，
  由末尾 `[FATAL] cli;hlt` 改为 `sched_kill(current)` 降级杀进程（与 CPL=3 用户越界路径同构）。
  - **不影响栈按需生长**：STACK_GROWTH 在 CPL=3 分支先行处理，新分支在 CPL=0 路径，其后执行；
  - **不影响懒分配**：懒区 `[0x40000000,0x41000000)` 在用户半区判定之外；
  - 仅"用户半区"例外降级，其余内核 bug 级缺页仍停机以便诊断。

**Tests**

* [abuse.c](../../v2-c-kernel/src/app/abuse.c) 新增两门禁：
  - **C1** `print@MMIO 0xFEB00000`：设备 MMIO 恒在用户上界之外，必被区间拒绝——布局不变量护栏；
  - **C2** `write deepcopy len=65536`：合法地址 + 64KB，预期绝不 `[FATAL]`，非全映射即逐页拒绝 / 全映射则流式搬运。
* qemu\_regression / test\_serial / rp\_torture 仍以 `[abuse] verify OK` 为哨兵，任一门禁漏放即判红。

**自测实录**

* 真实修复下：C1/C2 与 V5/V6/V7 全 `(rejected)`、`[abuse] verify OK`；`make test-host` pass=20 fail=0；
  `make test-qemu` 全量通过（含 stackovf/STACK\_GROWTH 演示路径不误伤）。
* 降级自验（临时跳过 fs\_write 预检模拟盲区）：`[kern] PF user-half @80500000 … -> kill pid=5`、
  `FATAL=0`、tick 心跳持续 → 整机存活。

Docs：bugs.md 新增 BUG-055；design.md 校验层/审计段见 BUG-054 表述，pf\_handler 降级语义见 BUG-055。

## [v1.4.8] - 2026-09-03 · syscall 用户指针校验上移单一收敛点 user_ptr_valid（BUG-054 二轮）

> 把"区间内空洞页"的逐页 `is_mapped()` 预检，从 copyin/copyout/copyin_str 三个 helper
> 上移到 **`user_ptr_valid` 单一收敛点**。前一轮（PR #53）漏掉的 6 处"仅调 user_ptr_valid
> 就直接解引用"的 syscall（fs_write/fs_read/readline/wait status/exec argv/recvfrom iov.buf）
> 现在全部在触碰用户内存前即被拒绝——实测这三条仍能整机壳死的路径（V5/V6/V7）全部封堵。
> 以后新增 syscall 只要调 `user_ptr_valid`（或 `copyin/copyout`）即天然自带"状态空间穷举"防御。

**Fixed**

* 预检收敛：`user_ptr_valid` 区间/回绕判定后追加逐页 `is_mapped()` 检查，空洞页在触碰前返 0；
  `copyin/copyout` 删除重复的本地逐页 helper（受益于收敛点），`copyin_str` 沿用跨页检查
  （`last_pg` 初值 -1 已正确覆盖非页对齐起始页）。逃逸面从"4/7 入口封堵"收敛为"全部入口"。
* 触发补证（外部 A/B 的 V5/V6/V7）：`fs_write(fd, 空洞, 8)`、`fs_read(fd, 空洞, 8)`、
  `fs_write(fd, 合法buf, 32768)`（普通写法越界，无需恶意）修复前均实测 `[FATAL] cli;hlt`
  整机停，是上轮"fs iov 走 copyin 一并封堵"表述对 fs 不成立的实证。

**Tests**

* [abuse.c](../../v2-c-kernel/src/app/abuse.c) 新增三门禁：`write buf@hole 0x80500000`、
  `read buf@hole 0x80500000`、`write buf@valid huge len=32768`（对应 V5/V6/V7），
  qemu\_regression / test\_serial / rp\_torture 三门禁一旦回归即 verify OK 不到 → CI 判红。
* [test_userptr.c](../../v2-c-kernel/tests/test_userptr.c) 的 `is_mapped` stub 升级为"假页表"
  （仅低区 + 末页映射），新增空洞页 / 已映射区上界 / 跨页进空洞 / 连续已映射页 / 末页 / len=0
  拒绝与放行断言，宿主层直接覆盖新预检路径。

**自测实录**

* `make test-host` pass=20 fail=0（test_userptr 28 断言全过）。
* `make test-qemu` 全量交互回归通过（含 exec 参数/forkdemo/fsdemo/wait/ccboot 合法路径不误伤）。
* 修复前 A/B：`run abuse` 打到 write@hole 即 `[FATAL] @80500000`、verify OK=0；修复后 FATAL=0、
  verify OK=2，全部边界用例 `(rejected)`、合法路径 write/read=8。

Docs：BUG-054"修复"与"回归"两节改写为两轮收敛描述；design.md 校验层同步"区间+逐页映射"与
END=0x81000000 常量。

## [v1.4.7] - 2026-09-02 · 压测壳修复：确定性判定空判据 + 打点节奏，落地"结果集复原"语义

> 用 record/replay 地基做**业内最佳实践测试**（`tests/rp_torture.sh` 新增：确定性差分 + 压力/边界
> 扫描 + 结果集复原）。初测抓出**两处测试壳"假绿"缺陷**（非内核），修复后两轮 `-icount` 冷启
> 确定性成立、内核无意外缺陷标记、现场可复原。这正是"用录放基因为测试本身照镜子"的成果。

**Fixed**（Tests，dev 侧基建）

* **空判据假绿**（`tests/rp_torture.sh` 的 `func()`）：旧实现 `grep -aE "…"` **未把文件参数 `$1`
  传给 grep** → grep 读空 stdin → `runA.func`/`runB.func` 恒空文件 → 确定性判定是对"两个空文件"
  比对，**无条件 PASS**。修复：`func(){ … "$1"; }`，判定真实落到输出文件上。审计后 `norm()`/`kmark()`
  均已正确使用 `$1`。

* **打点节奏缺陷**（`boot_battery`）：旧命令集 26 条连续 `tr_send` 无间隔灌入，shell 异步跟不上；
  末尾固定 `sleep 3` 快照把末条 `run hello` 掐在半路（runA 末 echo `o`/runB `run de`），且并发
  继承 demo 的 PID 顺序随之抖动（`[fork] pid=8` vs `pid=9`）。修复：改为**提示符同步打点**
  `tsend`（每条等下一个 `mini-os$ ` 再发下一条，计数自增），命令完成确定、尾部不再依赖盲 sleep。

* **复原判定语义**（新增 section D）：跨打点路径（record 用提示符同步 / replay 用 in.tr 相对毫秒）
  叠加并发继承 demo 的 icount×host 调度，`isol` 子进程 `ISOLATED OK` 与 shell `exited code=0`
  尾部两行偶发对调 → GO/NO-GO 改用**结果集相等（排序后）**，顺序差单列已知边界提示；避免把
  顺序噪声误报为重构/回归。

**Engineering**（Tests，dev 侧基建）

* `tests/rp_torture.sh`（新增，`record/replay 工作流实战`，用法 `bash tests/rp_torture.sh`）：
  A 两轮 `-icount` 冷启功能契约差分 · B 内核致命/越权/溢出标记扫描（区分预期 procCrash 隔离演示）·
  C tr2sqlite 检索 · D 重放黄金 transcript 做结果集复原。

**自测实录**

* 修复后 A 段：两轮 `-icount` 冷启**功能契约逐字节一致**，out\_lines 均精确 =1165 行。
* B 段：唯一命中 = `[user] PAGE FAULT pid=5 … -> killed`（procCrash 故意越权，前置 `crash demo`
  行标注为预期隔离演示）；无意外缺陷标记；churn 后 `free=62668KB` 稳定，无泄漏迹象。
* D 段：重放黄金 transcript 复现全部 **24 条** ISOLATED OK / exited code（结果集一一对应），仅
  `isol` 尾部两行顺序偶发对调（已知边界）。

Docs：本条目（roadmap 见 v1.4.4/4.5/4.6）。

## [v1.4.6] - 2026-09-02 · in.tr TSV 防御：`tr_send` 拒绝含 TAB/换行的 payload

> **预防**已指出的隐患：若未来 `.in.tr` 走 heredoc 多行 payload，混入原始 TAB/换行会破坏
> TSV 列分隔（`seq \t rel_ms \t payload`）。当前单行 writefile 确无此输入（暂无实际触发），
> 但为杜绝"坏 TSV 证据"静默深入，在**唯一写入钳制点**（`tr_send`）加 fail-fast 守卫。

* `tests/transcript.sh`：`tr_send` 检测到 payload 含原始 TAB/换行即**拒绝（不发送、不记录、rc=1）**，
  并 `%q` 报出违规命令 + 单行规约提示；拒绝写入 → 归档 TSV 永不脱列，差分/回放不会拿到坏列。
  实测：单行照常记录（`1 <tab> 6 <tab> ls /`），含 TAB 与含换行的 payload 各被拒（rc=1）、
  `seq` 不递增、`in.tr` 无污染。

* `tr_start` 文件头新增规约行：`payload 禁原始 TAB/换行，须单行；多行需显式编码 \t\n\\ 并同步回放解码`
  ——"为什么被拒"在证据文件里就地可见，自解释。

* **未来多行扩展点**（仅预言，未实现）：真需 heredoc 时再引入显式转义（`\\`/`\t`/`\n`）+ replay.sh
  同型解码后放开守卫；保证录放主路径仍 `.in.tr` 文本"证据原件"，且不回归现有单行差分。

* 自测：`make test-tr`（P2）/ `test-rp`（P3）全绿，守卫对合法单行零影响。

Docs：本条目。

## [v1.4.5] - 2026-09-02 · record/replay 接 repro\_bugs.sh：BUG 复现命令流固化为可回放证据

> 把 BUG-A（文件槽泄漏）/ BUG-B（cc500 argv）的复现脚本接进 P2/P3 录放机制——修复版复现
> 命令流经 transcript 录制固化为 `.in.tr/.out.tr`（补上 roadmap 指出的"repro\_bugs.sh 只脚本化、
> 未录时间关系"缺口），并实测可被 `replay.sh` 消费重放，形成首方复现回归。

**Engineering**（Tests，dev 侧基建）

* `tests/repro_bugs.sh`：`source transcript.sh`，`send()` 改调 `tr_send`（写 fd9 + 录制 in.tr 含
  **真实相对 ms**——wait 驱动的实际打拍也一并固化）；boot 后 `tr_start repro`，收尾 `tr_snapshot +
  tr_finish`（按 FAIL 状态标 RESULT PASS/FAIL）。QEMU 加 `-nic none`（非网络路径，去启动期 DHCP 开销）。

* `Makefile`：新增独立目标 `test-repro`（不在 `test:` 聚合内，语义同"首方复现/回归"）。

* **实测闭环**：录制生成 `build/transcripts/repro-<ts>/in.tr`（ccboot / exec / ls / writefile ×2 /
  ccrun ×3 共 9 条命令 + 真实相对 ms）→ `replay_into` 重新驱动内核 → 固定行为复现（`[ccboot]
  byte-identical PASS`、`out2.elf` 已建、`bad.c`→`cc500: error at`/`compile FAIL code=1`、
  good2.elf `exited code=0 PASS`）——BUG-A/BUG-B 在修复版均未复现。

**Docs**

* `roadmap.md`「record/replay 地基」：补 repro\_bugs 接入录放的说明。

## [v1.4.4] - 2026-09-02 · record/replay 地基工程收尾：无网络路径 `-nic none` + sqlite 分析索引

> P3 闭环后的两件零侵入加固：① 为回放/编译/录制这三条**不需要网络**的路径统一去掉默认网卡，
> 消除启动期 e1000/DHCP 等待（icount 下更省墙钟）并少一个非确定源；② 给 `.tr` 文本加一个
> **旁路 sqlite 分析索引**——录放主路径仍是文本"证据原件"，sqlite 只作只读"放大镜"，坏了绝不影响
> 录放正确性。

**Engineering**（Tests，dev 侧基建）

* `-nic none`：`tests/replay.sh` / `tests/test_transcript.sh` / `tests/test_cc500.sh` 的 QEMU 启动
  统一加 `-nic none`。实测内核无网卡时优雅跳过（`[net] e1000 not found on PCI` + `selftest skipped
  (no e1000)`，探针窗口内出 shell 提示符、不挂起）；网络回归（test\_net/tcp/socket/slip）保持
  `-netdev user -device e1000` 不动。

* **理由澄清**：`icount` 下 cc500 编译慢 + 后台 `[B]`/recvfrom 抢 tick，是用户态 demo（procB /
  sockdemo）抢指令预算所致，**非网卡导致**；网卡只对启动期（默认 e1000 + DHCP 握手）有墙钟贡献。

* `tests/tr2sqlite.py`（新增，分析索引"放大镜"）：把 `*.in.tr` / `*.out.tr` 增量导入 sqlite。
  三表 `transcripts`（元数据/血统）/ `in_events`（seq/rel\_ms/cmd/payload）/ `out_rows`（输出逐行）。
  **幂等 DELETE+INSERT**（按 runid，可重跑/增量补）；**只读旁路**——不读不写 `.tr`。用例：
  `python3 tests/tr2sqlite.py --dirs build/transcripts build/transcripts.sqlite --demo`
  （跨 runid 命令直方图 / 时间跨度 / 提示符计数 / FAIL 血统 / `LIKE` 检索编译结果行如 `cc500: error at`）。

**Docs**

* `roadmap.md`「record/replay 地基」：补充"无网络路径 `-nic none` + sqlite 分析索引"工程收尾说明。

## [v1.4.3] - 2026-09-02 · record/replay 地基 P3：replay 回放差分闭环（`make test-rp`）

> P1/P2/P3 地基三元闭环：icount 定内核确定性（P1）→ transcript 录输入/输出（P2）→ 回放消费
> transcript 驱动内核并证 bug 表现（P3，本轮）。

**Engineering**（Tests，dev 侧基建）

* `tests/replay.sh`（新增，回放器）：`replay_into <in.tr> <out.log> <runid> <done_regex>` 按
  seq/rel\_ms/payload 打拍注入串口、等完成信号驱动真实内核路径。**不用"日志静止"作结束判据**
  （本内核有后台 demo 应用持续打印）。

* `tests/test_replay.sh`（新增，`make test-rp`）：bug 本质闭环——从 bugs.md 抽 **BUG-026**
  （cc500 形参列表 EOF 未闭合→死循环），录 `writefile` 写 `int main(int x` + `ccrun` 的
  transcript → 回放 → 修复版见 `cc500: error at`（exit(1)，不死循环）。

* **诚实发现**（P3 实测边界，已规避）：icount(TCG) 下 cc500 编译分钟级 + 后台 demo 抢 tick →
  回放不用 icount，bug 闭环靠信号断言（逐字节确定性由 P1 test-det 承担）；跨独立冷启里程碑
  一致不机械稳定 → 两遍一致性作 `REPLAY_VERIFY=1` soft 检查。

## [v1.4.2] - 2026-09-02 · record/replay 地基 P2：transcript 固化（`make test-tr`）

> 承接 P1（icount 确定性）的录制侧：把串口输入命令流与输出字节流固化为可归档、可复现、
> 可差分的 transcript，失败自动归档现场。为 P3 回放差分铺数据源。

**Engineering**（Tests，dev 侧基建）

* `tests/transcript.sh`（新增，录制内核，`source` 用）：`tr_start/tr_send/tr_snapshot/tr_abort/
  tr_finish`。`*.in.tr` 列=序号/相对ms/命令（可重放审计），`*.out.tr` 原始字节流，`RESULT` 标
  PASS/FAIL 及失败点。

* `tests/test_transcript.sh`（新增，`make test-tr`）：验收三连——① 成功固化产物完整；
  ② **失败自动归档**（`tr_abort` 固化现场并标 FAIL，"人为触发失败可得可复现 transcript"）；
  ③ 复现性雏形（两次冷启同命令集，里程碑语义行逐字节一致）。

* **诚实发现**：非 icount 两次运行 `Hello ticks=296/297` 差 1——guest tick 随墙钟调度浮动，
  印证 roadmap"公共时钟须用 icount 虚拟时钟、非 guest tick"；复现性按 `ticks=N` pin 掉噪音，
  真逐字节确定性交给 P1 test-det。相对 ms 用 host 墙钟，icount 锚点留 P3。

## [v1.4.1] - 2026-09-02 · record/replay 地基 P1：icount 确定性启动验收（`make test-det`）

> 承接 roadmap「阶段二·加固」record/replay 地基的第一档：先用 `-icount` 把**内核执行**钉到
> QEMU 虚拟时钟，证明"同输入同输出"，为后续 P2 transcript 固化 / P3 回放差分铺确定性锚点。

**Engineering**（Tests，dev 侧基建）

* `tests/test_determinism.sh`（新增，`make test-det`）：两次 QEMU `-icount shift=auto,align=on,
  sleep=on` 冷启动，串口日志**逐字节 diff** 判定确定性——定时器/中断/调度/网络握手同输入同输出。

* **实测铁证**：icount 下启动段（含 **DHCP OFFER/ACK 网络握手**）两次运行逐字节一致。

* **诚实发现**：交互回归脚本（qemu\_regression 的 HMP sendkey / serial / persist / cc500）基于
  host 墙钟轮询（`wait_for`/`sleep`），与 icount 虚拟时钟流速不匹配 → icount 下 run 窗内超时
  误报。此即 roadmap P1 所述"暴露交互脚本的 host 墙钟时序依赖"，故**不回编**这些脚本；icount
  确定性验证独立收编为 `test-det`，交互确定性留待 P2 transcript 录放。

**Docs**

* `roadmap.md`「record/replay 地基」：P1 标记 ✅ 落地，记录实测结论与边界降级，P2/P3 仍待做。

## [v1.4] - 2026-09-02 · shell writefile heredoc 多行写入（绕开 128B 单行截断）

> 面向"AI agent 在 guest 内写较大源码"：`writefile <<DELIM <path>` 多行写入，逐行收集直至
> 独立 DELIM 行、逐行追加（每行≤128B 但任意行数拼接），大程序一次写入。128B 单行物理上限保留
> （键盘行缓冲），但不再是"只能写小 demo"的天线。这是把单行写盘升级为可用的源码写入通道。

**Added**（`src/app/shell.c`，`cmd_writefile`）

* **heredoc 多行模式**：检测 `args` 以 `<<` 开头 → 解析 `<<DELIM <path>` → create/open → 循环
  `sys_readline` 逐行收集，独立 DELIM 行（去首尾空白后精确匹配）终结；每行 `SYS_FS_WRITE` 追加
  其内容 + `\n`（空行保留行结构）；输出 `[writefile] '<path>' wrote <N> bytes (heredoc)`。

* **`cmd_help`**：补 `writefile <<D <p>` 语法行。

* **`shell_heredoc.h`（新增）**：DELIM 终结判定抽为**纯函数** `wf_delim_hit`（无 syscall 依赖，
  可宿主单测），heredoc 循环改为调用之。

**Fixed**（PR #25 审核发现）

* **DELIM 终结判定用错长度变量**：原实现 path 解析复用 `j` 且未保存 DELIM 长度——终结比较
  `s + j == e` 用 path 长度(如 8)而非 DELIM 长度(如 3)，EOF(3 字符)永不匹配 → heredoc
  **永不终结**，后续命令（ccrun 等）被吞进收集循环，serial 层 + 全链 CI 同点红（坑 5 稳定失败）。
  修复：path 解析前保存 `delim_len = j`，终结判定改用 `delim_len`，并抽出纯函数消除此类"变量
  复用"复发。

**Added**（Tests）

* `tests/test_serial.sh`：heredoc 回归用例——`writefile <<EOF /multi.c` 写 >128B 多行源码 →
  `ccrun` 编译运行 `exited code=0 PASS`（源码合法可编译运行，反证未被截断）。

* `tests/test_heredoc.c`（宿主单测，并入 run\_host\_tests）：DELIM 精确匹配 / 去空白 / 前缀不误
  终结 / 空行不终结 / **根因回归**（path 长度误当 DELIM 长度 → 判 0）——秒级锁定本 bug，防
  "本地未验证直达 CI"复发。宿主 20 项全绿。

**Docs**

* `bugs.md` OBS-004（=F-6）标记 **✅ 已缓解（v1.4）**：单行截断被 heredoc 绕开，仅保留"键盘单行
  物理上限"这一合理约束。

* `changelog.md`：本条目。

## [v1.3] - 2026-09-02 · 虚拟 TCP 上行滑动窗口（stop-and-wait→N 在途，吞吐 W/RTT）

> 上行停-等升级为**滑动窗口**：guest `tcp_send` 把载荷写入发送窗口（`TCP_TXWIN=8` 槽×独立 seq），
> 窗位有空即发、返回 `n` 即已入窗发往转发器；窗口满才让步等累计 ACK（host→guest `MSG_ACK` 的
> payload = 下一期望上行 seq，**累计确认**一次性推进 `tx_base` 多个槽位），最老未确认槽每
> `TCP_TX_TICKS` 超时重传。转发器为滑动窗口接收端（乱序包暂存 `up_buf` 凑齐连续后按序投
> `tcp.sendall`、回累计 ACK；重复 seq 覆盖不重投）。`MSG_ACK` 语义从"单报确认"提升为"累计确认"，
> 仍双向复用，**不新增消息类型、不改协议头结构**。吞吐从"1/RTT"提到"W/RTT"。

**Changed**（guest 薄包装，`src/app/tcp.h`）

* **tx\_conn\_t 发送侧重构**：移除 `tx_inflight`/`tx_inflight_len`/`tx_inflight_t`（停-等单槽），
  新增 `tx_base`（累计 ACK 边界，滑窗最老未确认 seq）、`tx_pending_start`、`tx_win[TCP_TXWIN]`
  （发送窗口槽数组，各带 seq/len/tick/busy/data）；`tx_seq` 保留为递增分配器。

* **新增** **`tx_slot_t`** **结构体**：承载单槽 seq/len/tick/busy/data，`tx_win` 下标 = `seq % TCP_TXWIN`。

* **`TCP_TXWIN`** **宏**（`src/app/tcp.h`）新增：`#define TCP_TXWIN 8`，上行发送窗口最大在途包数。

**Changed**（guest 薄包装，`src/app/tcp.c`）

* **`tcp_send`** **重写**：v1.3 起为"入窗即发出"的滑窗可靠发送——载荷写入 `tx_win[tx_seq%TCP_TXWIN]`，
  窗位有空即发、返回 `n` 即已入窗发往转发器；窗口满则阻塞让步等累计 ACK 推进 `tx_base` 后继续。

* **新增** **`tx_inflight`** **辅助函数**：返回 `tx_seq - tx_base`（在途未确认包数）。

* **新增** **`tx_retrans`** **辅助函数**：最老未确认槽（`tx_win[tx_base % TCP_TXWIN]`）每 `TCP_TX_TICKS`
  超时重传（SR 风格、幂等，转发器遇重复 seq 丢弃并回累计 ACK 自愈）。

* **新增** **`tx_ack`** **辅助函数**：累计 ACK 推进——`next` 落在 `(tx_base, tx_seq]` 内时，清被确收槽
  busy、`tx_base = next`；重复/越界 ACK 忽略。

* **`drain`** **中 MSG\_ACK 处理**：从"单报确认比较"升级为调用 `tx_ack(c, next)`（累计推进）。

* **`tcp_recv`/`tcp_wait_open`** **循环**：加 `tx_retrans(c)` 调用，确保 app 转入收读阶段仍在途包
  仍可超时重传。

* **`tcp_open`** **初始化**：`tx_inflight_len`/`tx_inflight_t` 改为 `tx_base=0`、`tx_pending_start=0`。

**Changed**（宿主转发器，`tests/tcp_proxy.py`）

* **Session 上行状态**：`up_next` 保持为累计确认边界；新增 `up_buf = {}`（乱序包暂存 dict）。

* **Proxy 类**：新增 `UP_WIN = 8` 常量（与 `TCP_TXWIN` 对齐）。

* **`MSG_DATA`** **上行处理**（`handle_msg`）：seq == `up_next` → 连续，转发到真 TCP、`up_next++`、
  排空 `up_buf` 中已凑齐的连续包，回累计 ACK；seq 在窗口内乱序 → 暂存 `up_buf`，仍回当前
  `up_next`（未推进）；超窗 → 丢弃，仅回 ACK。

* **`_up_ack`** **注释**：更新为"累计 ACK"语义。

**Added**（Tests）

* **`tests/test_upstream_window.py`**：宿主侧滑动窗口确定性单测——W=8 在序流水线齐发→累计 ACK 到 8；
  乱序 seq=9 被暂存 ACK 保持；补 seq=8 后累计跳到 W+2；重复 seq 不重新投递。验证：转发器乱序缓冲、
  累计 ACK 推进、去重均正确，上游 TCP 收到每块恰好一次按序。

**Engineering / Docs**

* `tcp-session-proto.md`：头部注记 v1.3 + §6 重写（6.1 下行可靠停-保持不变、6.2 上行滑动窗口、
  6.3 下行滑动窗口候选）；`MSG_ACK` 语义从"单报确认"提升为"累计确认"。

* `tcp-thin-api.md`：v1.3 注记 + tcp\_send 语义表更新为"入窗即发出"的滑窗可靠发送。

* `roadmap.md`：「上行滑动窗口」从候选标记为 **✅ v1.3 已落地**，新增"下行滑动窗口（候选）"。

* `changelog.md`：本条目。

## [v1.2] - 2026-09-02 · 虚拟 TCP 下行可靠（stop-and-wait）+ 大文件下载 demo + 路线图文档化

> 语义边界与演进方向写入 roadmap：下行可靠 + **上行可靠**停-等（v1.2 均落地）是"薄→厚"第一级
> 台阶（guest 收发两方向参与 seq/ACK，仍非完整状态机）；下一步候选「滑动窗口」列 roadmap。
> 上行可靠 + 传输封装附录在本条目一并记录。

**Changed**（wire，`src/net/tcp_proto.h`）

* **`MSG_DATA`** **flags 复用为"该方向序列号"**：会话头 flags 低 16 位由 v1.1 恒定 `0x0000` 改为
  按方向携带可靠 seq（host→guest = 下行 seq、guest→host = 上行 seq）——启用 v1.1 §2.1/§5
  预留的"为厚包装加序列号/ACK 占位"

* **新增** **`MSG_ACK`（0x08）且双向复用**：payload = 该方向"下一期望 seq"（2BE 大端）。guest→host =
  下行确认（推进转发器发下行）；host→guest = 上行确认（推进 guest 发上行）。同一类型按方向归属，
  不新增编号；移除 `tcp_parse_hdr` 的"flags 必须为 0"校验

* **msg\_type 方向合法性与** **`flags`** **规格更新**：`MSG_ACK` 双向合法（§2.3）；`MSG_DATA` 双向带方向 seq（§5）

**Changed**（guest 薄包装，`src/app/tcp.c/h`）

* **可靠下行接收**：`drain()` 只接受 `seq == rx_next` 的顺序包，推入 rxb 并回累计 ACK
  （`send_ack`）；重复/乱序包幂等丢弃并重发 ACK——端到端自愈；连接对象增 `rx_next` 期待序号

* **可靠上行发送（v1.2 新增）**：`tcp_send` 改为**阻塞停-等**——保存 `tx_inflight` 副本、分配上行
  seq（`tx_seq`）发 `MSG_DATA`，阻塞等 host→guest `MSG_ACK`（上行确认）后返回 `n`；超时按
  `TCP_TX_TICKS=2.5s` 重发在途副本（复用 seq，转发器幂等去重）。连接对象增 tx 状态字段；`sendpkt`
  拆出 `sendpkt_f` 支持携带 seq

* **demo**（`src/app/dldemo.c`）：真·大文件下载，不复用固定 `TCP_RXB`，每轮 tcp\_recv 取 2KB
  边收边累加，总长推至 **128KB 无总字节上限**，剥 HTTP 头后校验 body 尾 7B `EOFTAIL` 完整；
  注册进 initramfs，`DL_DEMO` 构建开关开机 spawn（Makefile / storage.c / kernel.c）

**Changed**（宿主转发器，`tests/tcp_proxy.py`）

* **可靠下行 stop-and-wait**：Session 增 `pending/inflight/seq/inflight_t/eof/finished`，
  in-flight 恒 ≤1 报，收到下行确认才发下一个（`_send_next`）；ACK 丢失按 `RETX_MS=2.0s`
  定时重发（`_retransmit`），SLIP 慢通道单报回环 \~1s，60ms 会灌爆慢 UART；e1000 快通道正常不触发

* **可靠上行（v1.2 新增）**：Session 增 `up_next` 计数器；`MSG_DATA` 上行按 up\_next 校验、顺序转发
  到真 TCP 并回 `MSG_ACK`（上行确认 `_up_ack`），重复/乱序去重不转发、重发 ACK；`parse_hdr` 返回
  `(sid, mtype, seq)`，`handle_msg` 增 seq 形参

* `_tcp_read` 改为读入 `pending` 后由 stop-and-wait 逐步下发，host EOF 标记后 data 发尽才发
  `MSG_CLOSED`——保证不缺尾

**Tests**

* **test\_tcp\_dl.sh**：宿主 128KB 文件服务 + QEMU 跑 dldemo，校验 200 OK / 总长==131072 /
  body 尾 `EOFTAIL` 完整；既有全量回归（test-tcp/slip-net/socket/persist/host）越跑越稳

* **test\_upstream\_reliable.py（新增）**：宿主侧确定性单测——伪造 guest 上传 3 块 + 注入重复块，
  断言转发器按序转发、重复去重、逐块回上行 ACK、上游收到完整有序字节（不依赖 QEMU）

**Engineering / Docs**

* `tcp-session-proto.md`：升 v1.2——`MSG_ACK` 双向 / 方向合法性 / flags 双向语义 / §6.1-6.2 上下行
  可靠（6.2 由候选转落地）/ 新增 **附录 A 传输封装规格**（第三方实现指南：e1000 与 SLIP 的端点、
  端口 7778、SLIP 帧内完整 IPv4/UDP、A.5 最小互操作清单 7 步）

* `tcp-thin-api.md`：tcp\_send 语义表改"阻塞停-等可靠发送"；v1.2 注记含上下行可靠、不破 API 互斥性

* `roadmap.md`：薄→厚演进候选标记「上行可靠 ✅ v1.2 已落地」，滑动窗口仍为候选

## [v1.1] - 2026-09-01 · 网络抽象层 netif + 虚拟 TCP 薄包装（Step 1-4）+ 收尾修复（PR #16）

> 把网络从"协议直接驱动 e1000"解开为三层：网卡适配层（e1000 以太 / 串口 SLIP）↔ netif 接口
> ↔ 协议层；虚拟 TCP 走"宿主转发器做真 TCP、guest 薄包装给连接对象 + 事件通道"。四步落地、
> 每步 CI 全绿，见 roadmap。收尾修复见 bugs.md BUG-044/045/046。

**Added**（路标 v1.1，Step 1-4，PR #11-15）

* **netif 抽象层**（`src/net/netif.c/h`，Step 1）：ops 表（init/ready/tx/rx/mac）+ 注册表，包单位=
  IP 数据报；以太网头 / SLIP 帧等链路层封装下沉到各网卡适配层（e1000 适配器 / `uart_netif` COM2）。
  协议层（src/net）不再出现任何具体网卡符号，`grep e1000_ src/net/` 为空由 CI 守卫强制（Step 3）

* **串口第二网卡**（`src/drv/uart_netif.c` + `net/slip.c`，Step 2）：COM2 走 SLIP（RFC 1055，
  END/ESC 转义）；`UART_NETIF_DEFAULT` 编译开关静态绑定网卡（D6）

* **会话协议**（`src/net/tcp_proto.h`，Step 4）：8B 会话头（session\_id/msg\_type/version/flags），
  大端；msg\_type 0x01 DATA / 0x02-0x05 事件(host→guest) / 0x06-07 控制(guest→host)；单一事实来源
  （guest C + 宿主 Python + fuzz 三方共用）。三份语义规定定稿进 docs/（session-proto/thin-api/mtu-fail）

* **虚拟 TCP 薄包装**（`src/app/tcp.c`，Step 4）：用户态库，`tcp_open/send/recv/close` + `wait_open`，
  fd=连接对象（非裸整数），事件队列，`tcp_recv` 三态 >0/0/-1 互斥；`httpdemo` 开机自动 HTTP demo

* **宿主转发器**（`tests/tcp_proxy.py`，Step 4）：会话表 + UDP↔TCP 映射 + 事件回传 + 超时/半开/背压，
  支持 UDP(e1000)+SLIP(COM2) 双通道

**Fixed**（收尾，PR #16，见 bugs.md BUG-044/045/046）

* **大响应丢尾**（BUG-044）：NET\_RXMAX 512→2048、接收环 1024→4096、转发器下行分块 ≤1392、
  发送硬墙对齐传输钳制 1400——"换 2304 环全清"实为容量而非地址损坏，三条容量症状一次归并

* **串口通道饿死**（BUG-045）：转发器主循环阻塞 recv → 改非阻塞 + multiclient select，不再卡死
  chardev/udp 输入

* **CI 稳定误报"HTTP 未就绪"**（BUG-046）：test\_tcp 自检改 retry 循环 + 失败即退（`|| exit 1`），
  修掉 PR #16 自引入的"单次无等待 curl"时序容错 bug（审核确认非 flaky）

**Tests**：host 19/19（fuzz 8 用例/48 万解析 ASan clean、Step3 e1000 守卫）；test-tcp 双通道
（>1KB/尾字节==TAIL 完整性断言）；test-slip 串口网卡。注：PR #16 双通道全量需 qemu 环境复跑确认。

## [v0.33] - 2026-08-31 · 回归可观测性收口（F-4 行撕裂 / F-5 pid 静默）+ harness 语义 + CI 全链

> 与上一批（v0.32）同为"收尾-加固-沉淀"阶段：不给新功能，只让回归更可信、更可定位。

**Fixed**（见 bugs.md BUG-042/043）

* **F-4 selftest 汇总行撕裂**（BUG-042，`src/app/shell.c`）：`cmd_selftest` 的 PASS/FAIL 汇总行
  此前用多次 `sys_print` 拼，片段间可被内核异步打印（孤儿 reap / DHCP 续约）插入撕裂 → 整行锚
  定回归假阴性。改走 `nl_*` 缓冲原子行（一次 flush，与 netping/ccboot/writefile 同机制）

* **F-5 pid 表耗尽静默**（BUG-043，`src/kernel/sched.c`）：`alloc_pid` 耗尽静默 `return -1`，三处
  调用方对 `pid<0` 无声返回（A4 fork 炸弹先到的是无声槽耗尽而非有日志深拷贝 OOM）。`alloc_pid`
  增"每耗尽周期报一次" `[sched] pid table full`（防 spawn/bomb 风暴刷屏）；`sched_audit` 补
  `slots=%u/MAX_PROCS`

**Engineering**

* **harness 退出码语义统一**（T3）：7 个测试脚本统一"缺依赖 → `[ERR]` + exit 2"（此前缺 socat
  等会退化成全断言超时 `[FAIL]/exit 1`，环境病伪装代码病）。约定：`0` 全绿 / `1` 断言失败 /
  `2` 环境或依赖缺失（记于 changelog 头部，CI 显式将 `2` 标环境错误）

* **CI 全链 + 分步矩阵**（T4，审核方落地）：每步 `make test-*` 各自全量重建 → 单 job `make test`
  全 7 层门禁 + 失败上传 build-logs 工件 + `workflow_dispatch`；新增 `layers.yml` 并行矩阵（host/
  qemu/serial/persist/net/socket/cc500 每层独立 job 定位）；含 test-cc500 层

* **账本收口**（T5）：bugs.md 追加 BUG-042/043 与 OBS-003/004；版本串 v0.32→v0.33 并同步 motd
  断言；新增 `docs/external-reviews/` 外部评估报告索引（F-xx ↔ BUG 号 ↔ commit 对照）

## [v0.32] - 2026-08-31 · 修复 cc500 编译器三缺陷（F-3 字符串自噬 / F-2 未定义静默 / F-1 关系运算残缺）

> 三个缺陷均会破坏 guest 内"写-编-跑"教学闭环（静默误编译 / 编译器自杀 / 无声失败），
> 宿主 hostcc 复现实锤后逐条修复（见 bugs.md BUG-039/040/041）。

**Fixed**（全部在 `tools/cc500/cc500.c`）

* **F-3 未闭合字符串字面量 → 越界读写自噬**（BUG-039）：`get_token` 字符串读取
  `while(nextc!='"') takechar()` 无 EOF 守卫（C 子集无 `break`，用标志变量），未闭合输入到
  EOF 仍让 `token` 无限增长；`primary_expr` 解码 `while(token[j]!='"')` 无 NUL 守卫，越过
  token 尾部越界读写直到堆里偶遇 `"`。修复：两处加守卫，命中 NUL 未闭合即
  `cc500: bad string` 干净报错（前：guest 被内核击杀 exit=-1 / hostcc SIGSEGV rc=139）

* **F-2 只声明未定义函数 → 静默编出 "call 自身 ELF 头" 的废产物**（BUG-040）：纯原型声明
  走 `program()` 函数声明分支不触发 `sym_define_global` 回填，符号恒留 `'U'=code_offset`
  （0x800A0000），调用即 `PAGE FAULT`。修复：`be_finish` 收尾遍历符号表（锚点=名字 NUL 位），
  检出残留 'U' 且 `value != code_offset` → `cc500: undefined symbol`

* **F-1 关系运算残缺（只有 <=）+ error() 零诊断**（BUG-041）：`relational_expr` 只识别 `<=`，
  `<`/`>`/`>=` 缺失；`error()` 裸 `exit(1)`。修复：`error()` 打印 `cc500: error at <token>`；
  补齐四元关系运算（setle=0x9e/setl=0x9c/setge=0x9d/setg=0x9f，操作数序 objdump 实测锁定）；
  字符串解码补 `\n`/`\t` 常规转义

**Engineering**

* 收编 `tools/cc500/host_crt.c` 为 hostcc 基座（把 cc500 编成 Linux 宿主程序，缺陷与内核无关，
  秒级红绿 + gdb 可调）

* 新增 `tests/test_cc500.sh`（挂入 `make test`）：**症状对立断言**（"新症状必须出现 + 旧症状必须
  缺席"，杜绝假绿）——宿主 T 系列 8/8 + `<` 编码 0f 9c 锁定；guest ccboot 自举不动点 P1==P2 +
  关系运算 `<` 运行语义（源码 <128B 避开 F-6 writefile 行截断）

* 全回归绿：宿主 16/16 + QEMU(195 断言, 含自举) + 串口 + 持久化 + 网络 + socket + cc500 +
  `repro_bugs.sh`（BUG-031/032 双断言）；尺寸锚点 `entry=800a0054` 未动，cc500 产物 18079B→21283B
  随代码体积自然漂移（无硬编码字节断言）

## [v0.31] - 2026-08-31 · 内核资源归属收口：per-process fd 表 + socket 归属/回收

> 连续两项：把"全局单表 + 无归属 + 不随进程回收"的共享内核资源改造成"每进程私有 +
> 进程绑定 + 退出归还"，根治跨进程槽污染与资源泄漏。

**Changed**

* **per-process fd 表入 PCB**（`src/kernel/sched.h/usermode.c/sched.c`）：`pcb_t` 增
  `fd_table[FS_FDS_PER_PROC]`（`fs_file_t` 定义迁入 sched.h），fd 号从"全局约定号"改为
  "本进程私有号"；`sys_open/read/write/close` 一律在当前进程自己的 fd 表上做，`sys_fork`
  深拷贝子进程 fd 表、`exec/exit` 清本进程 fd 表——不再有 BUG-031 式的跨进程槽号互污染与
  异常退出泄漏（v0.30 方案是记 pid 归属清理，此举把它彻底收进 PCB）

* `userprog.c` 的 `procFSB` 改用与 `procFSA` 相同的 `fd=1` 打开自己的文件，作为 per-process
  隔离性的负对照演示

**Fixed**（见 bugs.md BUG-037/038）

* **F-0a socket 表退出泄漏**：`net_sock_t` 增 `pid` 归属，`terminate_current` 调新
  `netsock_close_pid(pid)` 归还其所有 socket——此前开 socket 不关即退出使槽位永久失踪，
  直到表满网络降级

* **F-0b 任意 close 可关内核 DHCP 保留槽**：`net_sock_t` 增 `reserved` 标志标记端口 68 槽；
  `case 33` 改为 `netsock_close_if_owner`——仅可关本进程打开、非保留的槽，保留槽拒绝关闭，
  DHCP 续约链不再被打断

**Engineering**

* **F-0c 观测收口**：`netsock_audit()` 并入 `kern_audit`（socket 表占用计数 + 保留槽恒计数），
  socket 创建失败加"表满"专项日志

* 新增 `tests/test_socket.sh`（挂入 `make test`）：F-0a 退出回收（leak2 后 netping 仍 PONG）+
  F-0b 保留槽防 close + 观测断言；`qemu_regression.sh` 的 `slot` 断言同步改 `fd`

* 全层回归绿；socket 攻击回归（F-0a/F-0b）在修复前后红→绿区分成立

## [v0.30] - 2026-08-30 · 修复工具链严重 BUG（文件槽泄漏 + 自编译产物丢 argv）

> 两个带复现的真 bug 由独立实操报告、逐条核验属实后修复（见 bugs.md BUG-031/032）。

**Fixed**

* **BUG-031：全局文件槽泄漏污染工具链**（`src/kernel/usermode.c` + `sched.c`）：
  `fs_files[8]` 全局表无进程归属、退出路径不清理——cc500 一次 parse error（裸 `exit(1)`
  跳过 `flush_output`）即永久占用 slot2，此后所有编译 `setup_output` 失败直到重启。
  修复：文件槽记**打开者 pid**（`fs_file_t.pid`），`terminate_current` 调
  `fs_files_close_pid(pid)` 按归属归还；**不**关闭其他并发进程的槽（首版"关全部槽"被
  repro 抓到误伤 procSemB 与 P1 并存的场景）

* **BUG-032：cc500 自编译产物静默丢 argv**（`tools/cc500/cc500.c` `be_start`）：
  入口桩裸 `call` 不编组 argc/argv，自编译产物 exec 带 argv 时静默走默认路径写
  /out.elf 且退出 0。修复：`call` 前把内核栈 `[esp+4]=argc、[esp+8]=argv` 压给首函数
  （`mov eax,[esp+8]; push eax` ×2），与 cc500 "首参 8(%esp)/末参 4(%esp)" 约定精确
  对齐；`e_entry` 不变，`call` rel32 回填偏移 85→95

* **BUG-033：`map_page_in`** **页表帧 OOM 写物理 0 破坏内核**（`src/mm/mem.c/h`，
  代码审查 P0）：新页表 `frame_alloc()` 失败返回 0 未检查 → `(uint32_t*)0` 清零低 4KB
  且页目录项指向物理 0；`pf_handler` 栈生长静默失败 → 假增长死循环。修复：`map_page_in`
  改返回 `int`（-1 页表帧 OOM），`pf_handler` 检查失败转 `STACK_BOOM` 并释放已分配帧

* **BUG-034：kb 行缓冲在** **`line_ready`** **期间仍追加输入**（`src/drv/kb.c`，代码审查 P2）：
  行就绪未取时新可打印字符追加到旧行后，`kb_line_take` 取行时丢失。修复：仅
  `!line_ready` 才入缓冲；`test_kb.c` 补用例 13

* **BUG-035：`fork_frames[24]`** **硬编码限制大进程 fork**（`src/kernel/sched.c/h`，=OBS-002，
  代码审查 P1）：深拷贝超 24 页即 `fork_oom`（bigdemo 28 页已超限）。修复：改 kmalloc
  动态数组（同 `own_frames`），`sched_fork` 先数页数再按需分配、退出 kfree

**Engineering**

* 新增 `tests/repro_bugs.sh`（QEMU 串口复现/回归）：BUG-A（good.c 编译 OK → 坏源
  FAIL → 同源 good2.c 再编译必须成功）与 BUG-B（ccboot 产 P1 → `exec /out.elf
  /cc500.c /out2.elf` → /out2.elf 必须被创建）双断言

* 修复后实测：`[ls] out2.elf size=19217`（argv 生效）；good2.c 二次编译
  `[ccrun] ... code=0 PASS`（槽不污染）；ccboot P1==P2 逐字节一致仍成立

* 五层回归全绿：宿主 16/16 + QEMU + 串口 + 持久化 + 网络（cc500 桩改动经自举
  不动点 + 全量 QEMU 复验）

* 独立评估 L-4/L-5：版本串单一来源——新增 `src/version.h` 的 `MINI_OS_VERSION`，
  内核启动横幅（kernel.c）/ shell banner / initramfs motd 统一取宏，两处回归断言
  （qemu\_regression.sh / test\_serial.sh）同步 v0.30；Makefile cc500 单文件豁免
  精简为 `-Wno-int-conversion -w`（GCC 8+ 两者并存均有效——GCC 14 中
  `-Wint-conversion` 是 permerror 硬错误，`-w` 压不住，必须显式 `-Wno-*`；GCC 13
  下 `-w` 单独够用是曾误删此项的根源，见 BUG-036）

* **BUG-036（v0.30 内部回归）**：L-5 误删 `-Wno-int-conversion` 只留 `-w` →
  GCC 14 环境 `make` EXIT=2 构建失败（-Wint-conversion ×13）；恢复该项后
  修复。记录于 docs/bugs.md，教训：`-w` 不压 permerror，GCC>=14 需显式 `-Wno-*`

## [v0.29] - 2026-08-30 · 加固：宿主侧 fuzz + 内核堆审计

> 阶段二「加固」首批落地：不给新功能，只增信心。

**Added**

* **宿主侧 fuzz**（`tests/fuzz_parse.c`）：确定性 PRNG（xorshift32，固定种子可复现）
  对纯逻辑解析模块注入随机路径/随机字节，ASan+UBSan 下验证"畸形输入被拒绝而不崩溃"：

  * 覆盖 `fs_walk`（随机路径：`/`、`.`、`..`、空白、超长、写/建/删/列混合）、
    `elf_load_range`（畸形头/段表越界读）、`net_eth_type` / `net_parse_arp_reply`、
    `ip_parse` / `udp_parse` / `icmp_parse`（帧内载荷指针越界）、`dhcp_parse_reply`
    （畸形选项长度）；FS 内存盘每 4096 轮重置防 inode/块耗尽

  * 缺省 60000 轮（36 万次解析调用），`FUZZ_ITERS` 可调；已集成
    `run_host_tests.sh` 强制回归（第 16 项）

* **内核堆审计**（`src/mm/heap.c/h` `heap_audit()`，挂入 `kern_audit`）：

  * 遍历 `block_t` 链表：校验 magic/free 一致性、size 上界，块数超上界即判 next 成环
    停止（防死循环）

  * 新增 `used_bytes` / `free_bytes` 记账计数器，与遍历统计对账——泄漏（块游离于计数
    外）、双重释放（计数提前减）、写越界破坏块头的场景都会使两者漂移而暴露

  * 报告碎片（空闲块数/字节）；宿主 `test_heap.c` + QEMU selftest 双重锁定

**Fixed**

* **BUG-029：`icmp_parse`** **短帧越界读**（fuzz 抓到）：`len < 14` 时 `frame + 14` 越过帧尾、
  `len - 14` 无符号下溢成巨大值，`ip_parse` 按巨大长度扫载荷 → 堆缓冲区越界读。修复：
  `icmp_parse` 开头 `if (len < 14) return -1`；`test_icmp.c` 补 13/0 字节短帧回归断言
  （BUG-029 已在 `docs/bugs.md` 记录）

* **BUG-030：fork 子进程在继承的已生长栈上继续递归被误判缺页**（回归盲区补格抓到）：
  `sched_fork` 把子进程 `user_esp_top/stack_bottom` 原样继承 → 子进程栈在**父进程栈槽**；
  `stack_guard_hit` 却按**子 pid** 反推槽位，子进程下探时 fault 判"槽外"（STACK\_OK）→
  走普通缺页被隔离终止。修复：槽位改由**实际栈位置** **`stack_bottom`** 推导
  （`stack_bottom & ~(USER_STACK_SLOT-1)`），普通进程=自身槽、fork 子进程=继承的父槽，
  两者皆正确；v0.15 边界语义由真实栈槽天然保持（`src/kernel/guard.c`）

**Engineering**

* **回归盲区补格**：新增 `deepfork`（已生长栈×fork）与 `deepexec`（已生长栈×exec）
  演示应用，挂入 qemu\_regression.sh / test\_serial.sh（交互 + 全量校验双层断言）；
  第三个盲区项「brk 收缩-再涨」由 heapdemo 既有 step 4 覆盖、「编译产物×持久化」
  由 test\_persist.sh 的 S10（writefile→ccrun→save→重启→run）覆盖

* `test_guard.c`：补 4 条 fork 继承栈守卫断言（旧 pid 推导逻辑下必失败）

* `test_heap.c`：宿主为 64 位、`block_t` 实际占 24 字节（`next` 8B），审计篡改用例用同构
  probe 结构 + `offsetof` 定位 magic 字段，跨 32/64 位宿主通用；并在高强度
  分配/释放（分裂/不分裂/合并/复用全路径）后断言 `heap_audit()==0`

* `qemu_regression.sh` selftest 断言新增 `[audit] heap ok`

* 验证：宿主测试 16/16 全绿；内核 -m32 编译零告警；QEMU 回归全量通过
  （含 deepfork/deepexec 组合 + selftest `[audit] heap ok`：
  `4 blocks, free 3 blocks/102320B used 16B pages=25`，used+free+4×16B 头 = 25×4096
  精确守恒）；串口回归 + ATA 持久化回归通过；200 万轮 fuzz 复核无崩溃

## [v0.28] - 2026-08-30 · DHCP 租期续约（T1/T2 renew，RFC 2131 §4.4.5）

**Added**

* **租期续约非阻塞状态机**（`src/drv/e1000.c`）：ACK 后记录租期并计算 T1=0.5×lease、
  T2=0.875×lease（tick 化，100Hz）；`e1000_dhcp_tick()` 由 timer 心跳每 tick 驱动
  （`timer_cb` 里、`sched_tick` 前，保证不被上下文切换跳过），状态机
  `RENEW_NONE / RENEW_SENT / REBIND_SENT / REACQ_OFFER / REACQ_ACK`：

  * 到 T1 发**单播 RENEW**（ciaddr=已租 IP，带 server id(54)+请求 IP(50)）；
    到 T2 仍未 ACK 升**广播 REBIND**（ciaddr，仅带 50，任意服务器可续，RFC 2131）；
    ACK 后重置定时器继续下一租期；NAK/超时 → 重新走 DISCOVER->OFFER->REQUEST->ACK
    重新获取 → 彻底失败回静态兜底

  * 每 tick 至多"发一帧 + 收一帧"，绝不在 ISR 上下文忙等

* **端口 68 专用 DHCP 接收端点**（`src/net/netsock.c` `netsock_dhcp_open/recv`）：
  用户 socket 的 recvfrom 会"排空"网卡（netsock\_drain 取走 NIC 环所有帧），无匹配
  本地端口的 DHCP 应答会被抢先丢弃（sockdemo 每 tick 轮询即踩中）；注册端口 68 的
  DHCP socket 后，分发路径把应答入其队列，续约 tick 经它读取（与用户流量共享分发）

* **续约帧构建**（`src/net/dhcp.c/h`）：`build_bootp` 支持 ciaddr 与
  with\_server\_id（REBIND 不含 54）；新增 `dhcp_build_renew`（单播）/ `dhcp_build_rebind`（广播）

**Fixed**

* **BUG-027：`sys_map_page`** **记账槽满时帧泄漏**：`map_frames[8]` 满后第 9+ 张帧被映射
  但不记账、进程退出不回收。改为**分配前拒绝**（`map_fcount >= 8` 返回 -1），诚实暴露
  教学上限而非悄悄泄漏

* **BUG-028：exec 路径泄漏** **`load_frames`** **记账数组**：v0.26#3 把 load\_frames 改为 kmalloc
  动态数组时，spawn 路径补了 `load_frames_free()` 但 exec 路径漏了——每次 exec（成功或失败）
  泄漏该数组。修复：`sched_exec` 复制进 `own_frames` 后 `kfree(frames)`（参数改非 const，
  语义=移交），exec 失败路径补 `load_frames_free()`

* **e1000\_dhcp\_tick 用户页目录下访问高地址 MMIO 缺页**：e1000 MMIO 位于
  0xFEB00000（PDE≥512），用户进程页目录只克隆低 1GB PDE；timer ISR 可能在任意用户进程
  上下文运行 → 访问设备寄存器即 `[FATAL] page fault @feb83818`。修复：tick 内临时切内核
  页目录（与 netsock 收发同款），用完切回（须在 sched\_tick 前恢复）

* **print\_ip 缺** **`& 0xFF`**：`(ip>>24)` 等未掩码导致 IP 显示成 `10.2560.655362.167772687`
  （预存在 v0.25 的显示 bug，功能不受影响；续约日志与 OFFER/ACK 均走该函数，一并修正）

**Engineering**

* 宿主单测 `tests/test_dhcp.c` 38→61 断言：RENEW/REBIND 帧结构——ciaddr 写入 BOOTP 头、
  RENEW 单播（dst\_mac/目标 IP）带 54+50、REBIND 广播仅带 50（无 54）、UDP round-trip

* `tests/test_net.sh`：用短租期内核（`make DHCP_RENEW_SECS=2`，Makefile `-D` 注入
  e1000.o）在秒级窗口内观察续约闭环——新增断言 `renew: sent RENEW (unicast)` 与
  `renew ACK`；DURATION 循环会因 sockdemo 提前 break，须等续约出现再杀 QEMU（否则
  早杀漏掉 tick=100 的首次 RENEW）；pcap UDP 计数 10→12（RENEW+ACK 双向）

* Makefile：新增 `DHCP_RENEW_SECS` 变量（缺省用服务器租期，SLIRP 为 24h）；
  test\_net 跑完恢复常规内核

* 代码审查驱动：design.md §3 明确**单核假设**并发模型（sem/msg 的 try+block 在单核 +
  关中断模型下安全）；shell.c `file_copy/equal` 标注全局 fs 槽位 1/2 占用（per-process
  fd 表 TODO 见 roadmap 支线 C）；bugs.md 新增 BUG-027/028 与 OBS-002（fork\_frames\[24]
  硬编码上限观察项，动态化列入后续版本）

* **项目方向调整**：roadmap 纳入"收尾-加固-沉淀"三阶段路线 + 红线清单（README 定位同步）
  ——功能闭环已达成，后续不追逐版本号，转向 fuzz/record-replay 加固与教学文档沉淀

* 五层回归全绿（宿主 15/15 + qemu + serial + persist + net）

## [v0.27c] - 2026-08-30 · 评估反馈修复（GCC 14 构建卫生 + brk 守卫口径 + 回归补格 + 文档回补）

**Fixed**

* **S8：`brk_pages_up`** **容量守卫记账口径**（`src/kernel/usermode.c` `sys_brk`）：原守卫
  `brk_pages_up(old,a) > USER_HEAP_PAGES - heap_fcount` 用"旧 brk→新 brk 跨度"判定，
  收缩后旧映射保留（`heap_fcount` 不减、帧从不释放），再涨过同一段会**重复计数**、
  在真实预算内误拒；映射页恒为 `[heap_base, top)` 前缀，故改为
  `(a - heap_base + 0xFFF) >> 12 > USER_HEAP_PAGES` 判定（目标 top 覆盖页数）。单调增长
  下两式等价，收缩-再涨下本式正确。当前 bump-only 场景不可触发（cc500 不 free），
  属潜在缺陷修复

* **S7 告警清零**：`src/app/abuse.c` 两处 `unsigned < 0` 死断言改 `(int)` 窄化（此前已修）；
  `src/app/cc500_crt.c` 的 `char** -> char*` 不兼容指针加显式 cast；
  Makefile 给 `cc500.o` 规则加 `-w`（该源受 CC500 子集约束、无 cast 可用，
  `in_data == (0-1)` 惯用法的"comparison between pointer and integer"告警无专属
  -Wno 旗标，只能全量抑制）——恢复"零告警"卫生标准

**Engineering**

* **S10：persist 回归补「工具链 × 持久化」组合格**（`tests/test_persist.sh`）：
  第 1 次运行 `writefile /persist/p.c` + `ccrun /persist/p.c /persist/p.elf` → `save` 落盘；
  第 2 次重启 `run /persist/p.elf` → 编译产物仍可被加载运行（跨子系统回归盲区补格）

* **S2：design.md 文档回补**（停滞 10 版后补齐）：
  §2 内存布局总表更新（栈区 0x80010000-0x80090000 / shell 0x80090000 / app 1MB 0x800A0000 /
  共享内存 0x801A0000 / 堆区 0x801A4000 320KB / USER\_SPACE\_END 0x81000000）；
  §10 测试层数四层→五层（+网络回归）+ selftest 计数 5→6；
  新增 §17 网络子系统（v0.18-0.25）、§18 容量三连（v0.26）、§19 工具链与自举（v0.27）

* **S3：README 版本矩阵**补 v0.10-v0.27b 行（此前停在 v0.9，连续第 3 次被点名）

* GCC 14.2 clean build 验证：`make` 零告警，`kernel.elf` 329KB；五层回归全绿
  （宿主 15/15 + qemu 176 项 + serial + persist（含 S10 新用例）+ net）

## [v0.27b] - 2026-08-30 · 写-编-跑演示闭环：cc500 支持命令行路径

**Added**

* **cc500 命令行指定输入/输出路径**（`tools/cc500/cc500.c`）：`cc500_main(argv, argc)`
  按 `argv[1]=输入 argv[2]=输出` 取路径（`load_ptr` 逐字节拼回 4 字节指针，因 CC500
  无 int\* 解引用/类型转换），缺省回退 `/cc500.c` → `/out.elf`（`run cc500` 保持原行为）；
  入口/CRT 声明顺序对齐 CC500 反向压参与内核 `[esp+4]=argc,[esp+8]=argv` 的差异

* **shell** **`writefile <path> <content...>`**：把命令行剩余部分（保留空格）写入文件，
  让 agent 能在 guest 内经 shell 写源码（ARG\_MAX 32→128 解除内容截断）

* **shell** **`ccrun <src> <out>`**：fork+exec cc500 编译 `<src>` 为 `<out>` → `run <out>`
  → 校验退出码，端到端「写-编-跑」一键命令

**自举仪式（验收达成）**

* 完整演示剧本在 guest 内跑通：`writefile /hello.c <C 源码>` → `ccrun /hello.c /hello.elf`
  → 编译产物被加载运行（`[elf] '/hello.elf' loaded`）→ 程序输出 + 退出码 0；
  cc500 对**任意**合法源程序编译出可运行 ELF，不再局限于编译自身

**Fixed**

* BUG-026：cc500 对畸形输入（形参列表 EOF 未闭合）死循环——`program()` 形参循环
  `while (accept(")")==0)` 在 token 变空后无限调用 `sym_declare("")`，符号表
  `table_pos` 无界增长直至越界缺页。修复：缺名字/形参处遇 EOF 直接 `error()`

**Engineering**

* 页错误日志增强：`pf_handler` 附打印 fault EIP / eax / ebx（`PAGE FAULT … eip=… eax=…`），
  便于定位用户态故障指令（本次调试 cc500 越界即靠此定位）

* 回归升级：`tests/test_serial.sh` 新增 writefile + ccrun 用例（写源码 → 编译 OK →
  编译产物被加载 → PASS），与 ccboot（自举不动点）互补：前者证"能编译任意程序"、
  后者证"编译器对自身是不动点"

## [v0.27] - 2026-08-30 · 工具链与自举：guest 内「写-编-跑」闭环

**Added**

* **CC500 编译器移植**（`tools/cc500/cc500.c`，E. Grimley-Evans 自托管 C 子集编译器
  \~750 行）——v0.27-29「工具链与自举」的核心一步，**一步到位实现完整自举闭环**
  （原规划 27a 汇编器+链接器 / 27b C 前端 分两版，现以整机自托管编译器直接达成）

  * 链接基址改 `code_offset=0x800A0000`（APP\_LINK），ELF 头 e\_entry/p\_vaddr/p\_paddr
    同步改为 0x800A 基址，入口 stub 适配 mini-os ABI（`SYS_EXIT=0`）

  * **唯一机器码 stub = 通用系统调用** `syscall3(n,a,b,c)`（eax=n ebx=a ecx=b edx=c，
    int $0x80），`exit/malloc/getchar/putchar/sys_print` 全部改用 CC500 C 子集实现
    （malloc 基于 `SYS_BRK=35`），编译器内部不含任何平台相关代码

  * **输入输出走 mini-fs**：整读 `/cc500.c`（initramfs 预置源码）进堆做 `getchar` 源；
    `putchar` 为空操作，编译完由 `flush_output` 把 code 缓冲一次性写回 `/out.elf`
    ——绕开 mini-os 无 stdin/stdout 重定向的限制，与文件系统天然衔接

  * **专用 CRT**（`src/app/cc500_crt.c`）：`_start` 以 `cc500_main()` 返回值
    `sys_exit`（普通 crt.o 固定退出 0，无法把编译成败传给 shell）；不 include
    user\_lib.h（其 static inline `syscall3` 会与外部 `syscall3` 重名冲突）

* **initramfs 嵌入**：`cc500`（编译器 ELF）+ `cc500.c`（自举源，objcopy 原始字节）
  两个文件；Makefile 新增 cc500 构建链（`tools/cc500/cc500.c` → 独立编译 →
  专用 crt 链接 → ELF blob + 源码 blob 嵌入内核）

* **shell** **`ccboot`** **自举命令**：run cc500（gcc 版）编译自身 → `/out.elf`=P1；
  再 run `/out.elf`（=P1）编译自身 → `/out.elf`=P2；校验 P1/P2 的 FNV-1a 校验和
  与字节数一致，单行输出 `[ccboot] sha1=.. sha2=.. bytes=.. PASS/FAIL`

**自举闭环（验收达成）**

* guest 内验证：`cc500` 编译 `cc500.c` → P1（18079B，entry=0x800A0054）；
  P1 再编译 `cc500.c` → P2；P1 与 P2 **逐字节一致**（FNV 707789893 / 18079B）
  ⇒ 编译器对自身源码是"不动点"，自举成立；写文件→编译→运行闭环在 guest 内跑通

**Fixed**

* BUG-025：`sys_brk` 扩展时映射循环从非页对齐的 `old` 起逐 0x1000 上跳，brk 落在
  页中部时**顶部半页未映射**——任意非页对齐 malloc 都会越界缺页。heapdemo 用
  页对齐 sbrk 未暴露；cc500 任意尺寸 malloc 踩中。改为映射 `[old,a)` 相交的所有页
  （old 下取整、a 上取整），与 `brk_pages_up` 记账一致

**Engineering**

* 编译器 C 子集约束记录：无 `break/continue/for/switch/&&/||/!/</>/%/*(乘)/类型转换`，
  循环退出用 done 标志、`>=` 用操作数交换为 `<=`、换行用 `\x0a`（非 `\n`）——自举
  源本身必须能被自己编译

* Makefile 依赖陷阱：`$(KERNEL): $(OBJS)` 的 prereq 即时展开，`OBJS +=` 须在其前
  （否则 recipe 引用 blob 而 make 不先构建），已加注释

* 回归升级：`tests/qemu_regression.sh` 与 `tests/test_serial.sh` 新增 ccboot 用例
  （编译自身 OK → P1 加载 → 自举 PASS），版本横幅/motd 更新为 v0.27

* 全量回归（宿主 + qemu + serial + persist + net）五层全绿

## [v0.26] - 2026-08-29 · 容量三连#1：用户栈按需生长

**Added**

* **用户栈按需生长**（v0.26「容量三连」第一项）：每进程用户栈槽由 8KB 固定
  （守卫页 4K + 栈页 4K）扩展为 **32KB 槽 = 槽底硬底守卫页 4K（永不映射）+ 28KB
  可生长栈区**，栈从槽顶向下增长、初始仅映射顶页；命中守卫页时内核补映射新栈页、
  守卫页随栈底下移，直到槽底硬底（此时深越界才判溢出）

* **三态栈事件判定**（`src/kernel/guard.c` `stack_guard_hit`，纯逻辑可宿主单测）：
  由 v0.13 二态（0/1）扩为 `STACK_OK / STACK_GROWTH / STACK_BOOM`

  * `STACK_GROWTH`：fault 命中当前守卫页（= 当前栈页 `stack_bottom` 下方一页）
    且槽内仍有生长空间 → `pf_handler` 补映射、守卫页下移，指令重试

  * `STACK_BOOM`：fault 越过当前守卫页深越界，或已生长到槽底硬底仍下探 → 栈溢出

  * `STACK_OK`：fault 不在本进程槽内，或落在已映射栈页内（非栈事件，走原路径）

* **PCB 栈帧记账**（`src/kernel/sched.h` `pcb_t`）：`stack_frame` 单字段改为
  `stack_frames[]` 数组 + `stack_fcount` 计数 + `stack_bottom` 最低栈页地址；
  `sched.c` 统一经 `stack_init`（初始化槽顶/底/首页）与 `stack_free`（批量释放）
  管理，覆盖 spawn / exec / fork / reap 全路径

* **页错误栈生长处理**（`src/mm/mem.c` `pf_handler`）：用户态缺页命中
  `STACK_GROWTH` 时分配物理帧、映射到新栈页、更新 PCB 记账并打印
  `[stack] grow pid=… @… pages=…`，重试原指令；无帧/到硬底则转 `STACK_BOOM` 隔离终止

* **地址空间重布局**：栈区上移扩为 0x80010000-0x80090000，shell 迁至 0x80090000、
  app 槽迁至 0x800A0000、共享内存迁至 0x800A4000，避开扩展后的栈区

* **`deep`** **演示程序**（`src/app/deep.c`）：递归分配 1KB 局部数组 ×12 层，在 4KB
  初始栈上触发 3 次按需生长后存活（`survived 12KB recursion via stack growth`）

**Engineering**

* 宿主单测 `tests/test_guard.c` 重写（34 断言）：覆盖三态边界——槽外/已映射页内
  → OK、当前守卫页（有空间）→ GROWTH、深越界/到硬底 → BOOM、生长后守卫页下移

* 回归升级：`tests/qemu_regression.sh` 与 `tests/test_serial.sh` 新增 `deep` 用例
  （启动日志 → `[stack] grow` → 存活 → 退出码 0）

* 版本横幅与 motd 更新为 v0.26；roadmap 勾选该项

**Fixed**

* `deep` 演示程序尾递归被 `-O2` 改写为循环、栈占用不足 4KB 不触发生长（见 BUG-024）

## [v0.26#2] - 2026-08-29 · 容量三连#2：用户堆（brk/sbrk）

**Added**

* **用户堆系统调用**（`SYS_BRK=35`，`src/kernel/usermode.c` 分发）：`sys_brk(addr)`
  查询/设置 program break、`sys_sbrk(incr)` 相对增长（返回旧 brk）；堆区
  \[USER\_HEAP\_BASE, USER\_HEAP\_MAX) 在共享内存之后（v0.26#3 定为 0x801A4000-0x801F4000，
  320KB = 80 页），每进程独立、相互隔离

* **堆状态纯逻辑**（`src/mm/brk.h` `brk.c`）：`brk_pages_up`（扩展需补映射页数）、
  `brk_in_range`（收缩/复用边界）独立成模块，可宿主单测；扩展按页补映射物理帧并
  记账进 PCB（`heap_frames[]` + `heap_fcount`），收缩只更新 `heap_brk` 保留映射复用

* **页错误堆处理**（`src/mm/mem.c` `pf_handler`）：访问已分配但未映射的堆页时按需补映射，
  与栈生长共用页错误路径；堆页访问越界（越过 heap\_brk 且映射缺失）按普通缺页拒绝

* **`heapdemo`** **演示程序**（`src/app/heapdemo.c`）：初始 brk 查询 → sbrk(4K) 写入校验 →
  sbrk(16K) 写入校验 → 收缩回 8KB 处（内核保留映射）→ sbrk 复用已映射页 → 极简
  bump-allocator 冒烟（编译器 malloc 铺路）

**Engineering**

* 宿主单测 `tests/test_brk.c`（21 断言）：brk 状态机边界——不动/收缩/页对齐扩展/超上限

* 回归升级：`tests/qemu_regression.sh` 与 `tests/test_serial.sh` 新增 `heapdemo` 用例
  （brk 查询 → 扩页日志 → 收缩保留映射 → 存活 → 退出码 0）

## [v0.26#3] - 2026-08-29 · 容量三连#3：ELF 加载去上限

**Added**

* **ELF 加载去上限**：`usermode.c` 的 `load_frames` 由固定 8 项静态数组改为按需
  `kmalloc` 动态列表，`sched.c` 的 `own_frames` 同步动态化（`own_frames_take` 拷入
  PCB、`release_priv_frames` 归还），解除 32KB/8 帧约束，支持 MB 级 ELF；
  `APP_MAXFRAMES`/65536B 旧上限检查移除（上限改为 app 区同量级 1MB）

* **地址空间重布局**：用户空间扩至 16MB（USER\_SPACE\_END=0x81000000）；app 区扩为
  1MB（0x800A0000-0x801A0000），共享内存迁至 app 区之后（0x801A0000）、堆区迁至
  0x801A4000（v0.26#2 首版曾驻 0x800B0000，随本次迁址后断言同步更新）

* **`bigdemo`** **演示程序**（`src/app/bigdemo.c`）：70KB 初值数据（.data 段）使 ELF 文件
  78KB > 旧上限 65536B，加载需 21 帧（旧 8 帧上限时代无法加载）；逐字节填充校验和
  验证数据完好

**Engineering**

* 宿主单测 `tests/test_userptr.c` 边界更新：USER\_SPACE\_END 扩为 0x81000000 后
  上限内末 4 字节 / 越界 / 长度溢出断言随新边界调整

* 回归升级：`tests/qemu_regression.sh` 与 `tests/test_serial.sh` 新增 `bigdemo` 用例
  （启动 → 70KB 校验 → 存活 → 退出码 0），并断言加载帧数突破旧 8 帧上限

## [v0.25] - 2026-08-29 · DHCP 客户端：动态获取 IP/网关（静态可配置化）

**Added**

* **DHCP 协议**（`src/net/dhcp.c/h` 纯逻辑，可宿主单测）：BOOTP 固定头 + 选项
  （RFC 2131/2132）

  * `dhcp_build_discover`：0.0.0.0:68 → 广播 255.255.255.255:67，携带参数请求列表
    （option 55: 1 子网掩码 / 3 路由器 / 51 租期），flags=0x8000 请求广播应答

  * `dhcp_build_request`：携带 server id(54) + 请求 IP(50)

  * `dhcp_parse_reply`：校验 xid / magic cookie / 消息类型(53)，提取分配 IP(yiaddr)、
    server id(54)、网关(3)、租期(51)；xid 不匹配/缺 cookie/无消息类型/过短 → 拒绝

* **`e1000_dhcp_run`** **开机动态取 IP**：DISCOVER → OFFER → REQUEST → ACK 四步状态机
  （忙等超时 \~2s，不依赖 timer），NAK/超时自动重试，失败回退静态地址——
  静态兜底收敛为单一配置点 `NET_STATIC_IP` / `NET_STATIC_GW`（10.0.2.15 / 10.0.2.2）

* **e1000 提供 IP 访问器**：`e1000_my_ip()` / `e1000_gw_ip()`；ARP / UDP / ICMP
  三个 selftest 由硬编码 IP 改为取动态 IP（DHCP 学得或静态兜底），开机顺序为
  `e1000_init → e1000_dhcp_run → e1000_selftest → udp/icmp selftest`

**Engineering**

* 宿主单测 `tests/test_dhcp.c`（38 断言）：DISCOVER/REQUEST 帧结构/字段、UDP
  round-trip、OFFER/ACK 解析、网关提取、xid/缺 cookie/无消息类型/过短/op 错误全拒绝

* `make test-net` 回归升级：串口断言新增 DHCP 四项
  （DISCOVER 发出 / OFFER 收到 / REQUEST 发出 / ACK 收到），与 ARP/UDP/ICMP/
  sockdemo/netping 全链路端到端互通

* 版本横幅与 motd 更新为 v0.25；roadmap 勾选该项

## [v0.24] - 2026-08-29 · UDP 校验和错误路径

**Added**

* **接收端 UDP 校验和验证**（`src/net/udp.c` `udp_parse`）：伪头(12B: srcIP|dstIP|0|17|ulen)

  * UDP 头 + 载荷重算须折叠为 0 才接受；校验和字段为 0 视为"发送端未计算"
    （RFC 768 IPv4 允许），跳过验证直接接受

* **发送端 RFC 768 对齐**：`udp_build_frame` 算得校验和为 0 时以 0xFFFF 发送
  （0 是"未计算"标记，两者不再混淆）

* netsock 分发链路据此生效：坏校验和的帧在 `udp_parse` 即返回 -1，被静默丢弃，
  不进入任何 socket 队列（网络栈具备"丢坏包"的第一道完整性防线）

**Engineering**

* 宿主单测 `tests/test_udp.c` 追加 6 条断言（24→30）：载荷篡改 / 校验和字段篡改 /
  伪头 srcIP 篡改（重算 IP 头校验和后仍拒）→ 全部拒绝；校验和=0 → 接受

* `make test-net` 回归全绿：真实 SLIRP PONG 校验和有效，接收路径不受影响；
  与 ICMP/ARP/UDP 回环/用户态 sockdemo 端到端互通

* 版本横幅与 motd 更新为 v0.24；roadmap 勾选该项

## [v0.23] - 2026-08-29 · ICMP Echo：PING 通宿主

**Added**

* **ICMP 协议**（`src/net/icmp.c/h` 纯逻辑，可宿主单测）：Ethernet+IPv4+ICMP Echo
  请求/应答，校验和只覆盖 ICMP 报文本身（RFC 792、无伪头）

* `e1000_icmp_selftest` 开机自检：发 Echo 请求到 SLIRP 网关 10.0.2.2，收其回显应答
  （`[icmp] echo reply from 10.0.2.2 OK (rtt=N ticks)`）——补上"ping 即网络活"的经典语义

**Engineering**

* 宿主单测 `tests/test_icmp.c`（22 断言）：帧构建/解析回读、空载荷、校验和篡改拒绝、
  协议不匹配拒绝、帧过短拒绝

* `make test-net` 回归：串口断言 + pcap 独立核验 IPv4/ICMP 双向包 ≥2（Echo 请求+应答）

## [v0.22] - 2026-08-29 · 网络交互化：shell `netping` 命令

**Added**

* shell 内建 `netping [ip] [port]`（默认 10.0.2.2:7777）：开 UDP socket 发 PING、
  轮询收 PONG，单行原子打印 `[netping] <ip>:<port> PONG +<N>B rtt=<T> ticks`
  （IP 大端序正确显示）——把"演示程序"升级为"交互命令"，agent 可一键验证连通性

**Engineering**

* `make test-net` 六层 + HMP sendkey 交互注入 netping 断言；pcap UDP 4→6

## [v0.21] - 2026-08-29 · 内核自审计 + syscall 边界契约化

**Added**

* 运行时自审计内建进内核：`sem_invariant_ok`（count+waiters 守恒）、`mem_audit`
  （used\_frames 与帧位图配平）、`sched_audit`（PCB 状态机合法性），由 syscall 34
  `SYS_KERN_AUDIT` 一键触发

* selftest 追加第 6 项，`[selftest] PASS (6 checks)` 从"5 个应用没崩"升级为
  "内核核心不变量成立"

* 调度日志（block/wake/exit）统一带单调 tick 戳；abuse 边界断言补齐至 17 项
  （exec argv / sendto·recvfrom iov / read·write buf 等内核地址一律 -1）

## [v0.20] - 2026-08-29 · 网络可用：用户态 UDP socket

**Added**

* `sys_net_socket/sendto/recvfrom/close`（30-33）；内核 `netsock` socket 表 +
  网卡轮询分发（recv 先排空 NIC 再取队首，非阻塞与轮询驱动对齐）

* `netio.h` 共享 iov 结构（3 参 syscall 承载多参，ABI 与内核一致）

* `sockdemo` 用户态端到端回环（socket→sendto PING→轮询 recvfrom PONG）

**Fixed**

* e1000 MMIO 位于高地址（PDE≥512）、进程页目录只克隆低 1GB PDE 导致的 syscall
  路径缺页——netsock 收发前临时切内核页目录（BUG 见本版修正）

**Engineering**

* `make test-net` 升级六层（+sockdemo 断言 + pcap UDP≥4）

## [v0.19] - 2026-08-29 · 网络加厚：极简 IP/UDP

**Added**

* **极简 IPv4**（`src/net/ip.c/h` 纯逻辑）：头部构建/解析 + RFC 1071 16 位校验和
  （`ip_checksum`/`ip_build`/`ip_parse`，校验版本/IHL/总长/校验和）

* **极简 UDP over IPv4**（`src/net/udp.c/h` 纯逻辑）：完整帧构建（Ethernet+IPv4+UDP，
  校验和含伪头 12B）/解析（round-trip + 拒绝路径）

* 内核态 e1000 UDP 回环自检（经 SLIRP 到宿主 UDP echo 服务 PING/PONG）

**Engineering**

* 源文件按子系统分目录（arch/kernel/mm/drv/fs/net/app，Linux/MINIX 风格），
  头文件逐目录 -I，`make test-net` 起纳入 IPv4/UDP 断言 + pcap 独立核验双向包

* 宿主单测 `tests/test_ip.c`（24 断言）/ `tests/test_udp.c`（24 断言），
  基准校验和由独立 python 参考实现算得

## [v0.18] - 2026-08-29 · e1000 网卡驱动 + 极简网络栈（PCI/ARP）

**Added**

* **PCI type-1 配置空间访问**（`src/pci.c/h`）：`pci_config_read/write`（端口 0xCF8/0xCFC）、
  `pci_find(vendor, device)` 扫描总线 0 找到网卡、`pci_bar_alloc_mem`——探测 BAR 大小
  （全 1 写回再读）、在 PCI MMIO 洞（0xFEB00000 起）分配地址并写回、使能 MEM|BUSMASTER
  （QEMU `-kernel` 不经 SeaBIOS，BAR 须驱动自分配）

* **e1000 驱动**（`src/e1000.c/h`，Intel 82540EM / QEMU 默认网卡）：

  * MMIO BAR0 恒等映射进内核页目录；软复位（CTRL.RST）→ 强制链路（CTRL.SLU）→ 轮询 STATUS.LU

  * MAC 从 RAL0/RAH0 读取；legacy 16B 描述符环（RX16/TX8），轮询收发（无中断/DMA 中断）

  * `e1000_tx`：填描述符（EOP|IFCS|RS 等）→ 写 TDT 触发 → 轮询 status.DD 确认发送完成

  * `e1000_rx`：轮询当前描述符 DD → 拷贝缓冲 → 归还 RDT

  * 启动自检 `e1000_selftest()`：发 ARP 请求（who has 10.0.2.2）→ 收 SLIRP 网关回复，
    端到端验证 TX+RX；配合 QEMU `filter-dump` pcap 独立核验线上包

* **极简以太网/ARP 帧**（`src/netutil.c/h`，纯逻辑可宿主单测）：
  `net_build_arp_request`（广播帧构建）/ `net_eth_type` / `net_parse_arp_reply`

* kernel.c 接入：`e1000_init()` + `e1000_selftest()`（无网卡自动跳过，真机/无网环境不受影响）

**Fixed**

* BUG-018：`e1000_tx` 等待 DD 位 3M 次轮询总超时、pcap 无包——**描述符环非 volatile，
  GCC -O2 把 status 读提升到循环外**，轮询循环被优化成单次判断直接返回 -1；
  `tx_ring/rx_ring` 声明 volatile 且局部指针 `d` 带 volatile（仅数组 volatile 而指针
  丢弃限定符仍会复发）后恢复（见 bugs.md）

* BUG-019：TCTL/RCTL 的 **EN 位是 bit1（0x2）不是 bit0（0x1）**——`TCTL_EN=1` 写出的
  TCTL=0x9 的 bit1=0，QEMU `start_xmit` 判定"TX 未使能"直接返回（TDH 恒 0、TPT=0、pcap 空）。
  对齐 Intel 手册与 QEMU 定义（`E1000_TCTL_EN=0x2`）后 TX/RX 打通

* BUG-020：QEMU 特例——写 RCTL 会启动 1000ms 的 `flush_queue_timer`，期间收到的包
  被排队、不进 RX 环，自检前 1 秒轮询什么都收不到（多次重发才偶中）。e1000\_init 末尾
  等 flush 窗口过期再收发，自检一次通过（重试循环仍兜底）

**Engineering**

* 宿主单测 `tests/test_netutil.c`（44 条断言）：ARP 请求构建（广播/字段/长度）、
  ethertype 提取、ARP 回复解析（op/sha/spa）、畸形帧拒绝——纯逻辑与硬件解耦

* 回归升级为**四层 + 网络**：新增 `make test-net`（`tests/test_net.sh`）——
  QEMU `-device e1000` + SLIRP + `filter-dump` pcap；校验串口日志里程碑
  （e1000 探测+链路 / ARP 请求 / 收到 SLIRP 回复），并用 python 解析 pcap
  独立核验线上确有 ARP 双向交换（req≥1 且 reply≥1）

* 版本横幅与 motd 更新为 v0.18；Makefile 接入 `pci.o`/`e1000.o`/`netutil.o` 与 `test-net`

* 全量回归（宿主 + qemu + serial + persist + net）五层全绿

## [v0.17] - 2026-08-29 · syscall 边界校验（copyin/copyout）

**Added**

* **用户指针校验层** `src/userptr.c/h`（纯逻辑，可宿主单测）：

  * `user_ptr_valid(p, len)`：校验 `[p, p+len)` 完整落在用户空间
    `[USER_SPACE_BASE=0x80000000, USER_SPACE_END=0x80100000)`（含上界与回绕保护）

  * `copyin` / `copyout`：校验通过后内核直接拷贝用户内存（当前 CR3 即用户页目录）

  * `copyin_str`：把用户 NUL 结尾字符串拷入内核缓冲（非法基址/越界/超长返回 -1）

* **全部涉用户指针的 syscall 接入校验**（usermode.c）：

  * `sys_print`（拷贝进内核缓冲再打印）、`sys_readline`（缓冲校验）、
    `sys_spawn_file`（name 校验）、`sys_wait`（status 出参校验）、
    `sys_exec`（name + argv 数组 + 每个 argv\[i] 校验）

  * FS 全链路：`create/open/ls/delete/mkdir/rmdir` 的路径、`write/read` 的缓冲指针

* 演示应用 `src/apps/abuse.c`：用内核低地址（0x100000/0xB8000）与回绕地址
  （0xFFFFFFF0）调用各类 syscall，验证全部被拒（-1），合法路径不受影响
  （写文件返回正常字节数），最后 `[abuse] verify OK`

**Engineering**

* 宿主单测 `tests/test_userptr.c`（20 条断言）：`user_ptr_valid` 纯逻辑边界
  （起点/上限/END/内核低地址/地址 0/回绕/长度溢出）+ copyin/copyout/copyin\_str 拒绝路径

* 回归补 `run abuse` 用例（serial + qemu 双通道）：断言内核指针被拒 + verify OK

* 版本横幅与 motd 更新为 v0.17；Makefile 接入 `userptr.o` 与 `abuse` 应用

* 开发期自查修正：`user_ptr_valid` 首版 `len > END - a` 在 `a > END` 时减法回绕误判为
  合法，改为先 `a > END` 拒绝再判区间（未发布，已在本版修正）

## [v0.16] - 2026-08-29 · 用户态 CRT 收口 + ATA 真盘持久化 + 单行自检

**Added**

* **用户态 CRT 收口**：新增 `src/apps/crt.c`，ELF 入口由 `app_main` 提升为 `_start`——
  `_start(argc, argv)` 调 `app_main`，返回后统一 `sys_exit(0)`。根治"app\_main 忘了
  sys\_exit 从栈槽顶未映射处 ret 崩溃"这类问题（BUG-016），各应用不再手写尾部 `sys_exit(0)`

* **ATA PIO 驱动**（`src/ata.c/h`）：主通道 master、LBA28、轮询模式；IDENTIFY(0xEC)
  探测扇区数，读(0x20)/写(0x30) 按扇区，带 BSY/ERR/超时保护；无盘立即返回、回落纯内存盘

* **FS 持久化（分水岭）**：`src/storage.c/h` 存储子系统

  * 有盘：整盘读入 ramdisk → 超级块 magic 有效则**直接挂载**（磁盘即真源，用户数据跨重启存活）；
    空白盘格式化 + initramfs 并首启落盘一次

  * 无盘：纯内存盘（v0.8 原行为）

  * `SYS_FS_SYNC(29)` + shell `save` 命令：把 ramdisk 全量写回磁盘

* **单行结构化自检**：shell `selftest` 命令——逐跑 hello/isol/forkdemo/fsdemo/waitdemo
  （覆盖 spawn/隔离/fork/FS/wait），每项打印退出码，汇总一行 `[selftest] PASS (5 checks)` / FAIL，
  外部 agent grep 一行即完成全量验证

**Fixed**

* BUG-016：fsdemo 无 `sys_exit` → app\_main 返回从栈槽顶未映射处 ret → 页错误被误判为
  STACK OVERFLOW、退出码 -1（回归只 grep `[fsdemo] done` 而被掩盖）。v0.16 双管齐下：
  ① CRT 收口根除整类问题；② guard.c `stack_guard_hit(fault, pid)` 改为只认定"落在本进程
  守卫页"的 fault 才是栈溢出（槽顶边界归下一槽，不再误报）

* CRT 引入时的连带问题：spawn 路径入口改 `_start` 后，`_start` 读 `[esp+8]` 的 argv 越出
  已映射栈页 → shell 一启动即页错误；sched.c `entry_block` 把入口 cdecl 块写在栈页顶下方 12B 修复

**Engineering**

* 回归体系升级为**四层**，并新增 `make test-persist`：

  * `tests/test_persist.sh`：**两次 QEMU 运行共享同一** **`-hda`** **镜像**——第 1 次格式化空白盘、
    `mkdir /persist` + `save` + 退出；第 2 次重启挂载校验 `/persist` 仍在、持久盘应用可经
    `selftest` 正常运行（用户数据跨重启存活的铁证）

  * 断言补强（fsdemo 教训）：给 qemu 通道 fsdemo/waitdemo 补退出码断言；serial/persist/qemu
    三通道加入 `[selftest] PASS (5 checks)` 检查

* 回归盲区反思：关键字断言只能验证"某行出现了"，验证不了"退出码"这类不变量——
  文档新增"回归盲区的教训"，把可见输出匹配提升为退出码不变量校验

* 版本横幅与 motd 更新为 v0.16；Makefile 接入 `ata.o`/`storage.o`，应用链接改 `-e _start`

## [v0.15] - 2026-08-29 · 补全 wait()/waitpid 语义与孤儿清理

**Added**

* **`sys_wait(pid, *status)`** **升级为经典 wait/waitpid**：

  * `pid=-1`：等待**任意**子进程（`wait()`）；`pid` 具体：等待该子进程（`waitpid(pid)`）

  * 返回**被回收的子进程 pid**（无子进程/非法返回 -1），退出码写入 `*status` 出参

  * **只回收"自己的"子进程**（校验 `parent_pid == 当前进程`），不误收他人僵尸

  * 唤醒路径：`terminate_current` 同时唤醒"等任意"（`block_arg=-1`）与"等具体 pid"的
    等待者，唤醒时返回子 pid，并把退出码切到父进程地址空间写入 `*status` 出参

* **子进程孤儿化**：父进程退出时把所有子进程 `parent_pid` 置 0（交心跳回收），
  修复"父退出后其 pid 槽被复用、孤儿永远等不到父 FREE 而被回收"的潜在泄漏

* 演示应用 `src/apps/waitdemo.c`（原子行输出）：fork 3 个子进程（退出码 7/9/11），
  父进程循环 `wait(-1, &code)` 依次回收任意子进程，校验 pid 互异、退出码集合 {7,9,11}，
  全部回收后再次 `wait(-1)` 返回 -1

* shell 的 `run`/`exec` 适配新签名；`exec <不存在的程序>` 失败反馈用例
  （子进程 `[exec] FAILED to exec` → `sys_exit(1)` → 父进程 wait 拿到 code=1）

**Engineering**

* QEMU 回归新增 v0.15 检查项：waitdemo 父 fork / 三个 `wait any` 回收码 7/9/11 /
  verify OK / 无子进程返回 -1 / exec 失败反馈 / 内核 `wait any` 日志

* 串口终端回归补 `run waitdemo` 与 `exec nosuchprog` 用例

* Makefile/initramfs 接入 `waitdemo`

## [v0.14] - 2026-08-29 · 文件系统增强：目录层级 / 间接块 / 偏移定位与追加写

**Added**

* **目录层级**：`fs_mkdir` / `fs_rmdir`（仅空目录，非空/非目录拒绝）/
  `fs_lookup_in` / `fs_list_dir`；目录操作泛化为"指定目录 inode"，不再写死根目录

* **路径解析器** **`fs_walk`**：绝对路径 `/a/b/c`，支持 `.`、`..`（显式目录栈回退）、
  重复/结尾斜杠；根目录的 `..` 仍是根。`fs_create/lookup/delete/list` 全部路径化

* **间接块**：inode 增加 `indirect` 字段（存 1024 个块号的块），
  单文件上限从 12 块(48KB) 提升到 12+1024 块 ≈ 4.1MB；`fs_read/fs_write` 支持惰性
  分配间接块与数据块，删除时递归释放间接块及其指向的所有数据块

* **文件偏移定位与追加写**：`sys_fs_open` mode=2 追加（pos=文件尾）、
  新增 `sys_fs_seek(slot, off)`（SYS\_FS\_SEEK=26）/ `sys_fs_mkdir`(27) / `sys_fs_rmdir`(28)

* 目录条目增加 `type` 字段；`sys_fs_ls(path)` 路径化并按类型打印（目录带 `/` 标记）

* shell 新增命令：`mkdir <path>` / `rmdir <path>` / `rm <path>` / `ls [path]` / `cat <path>`（路径化）

* 演示应用 `src/apps/fsdemo.c`（原子行输出）：mkdir /etc、/etc/sub → 子目录建文件 →
  追加写两段配置 → seek 读回校验 "8080" → 100000 字节大文件（间接块）4 处偏移抽查 →
  rmdir 拒绝非空目录 → 逐级清理

* 宿主单测 test\_fs 新增 v0.14 用例：目录层级（嵌套/类型标记/非空拒绝/.. 与 // 解析）、
  间接块（100000B 写入/4 处抽查/997 步长全量抽样/超上限边界）——1182 → 8686 断言

**Fixed**

* BUG-014：`sys_wait` 的 spawn 后、wait 前竞态——子进程退出后被 `sched_tick` 抢先回收，
  父进程 wait 拿到 -1 而非真实退出码（v0.12 遗留，v0.14 修复）

* `fs_walk` 中间组件缺失时未写 leaf/dirout 导致调用方读取未初始化栈值（BUG-015）

**Engineering**

* 修复测试/演示的确定性：

  * msg 演示生产者首发前睡 12 tick，保证消费者先 recv 阻塞（否则交错依赖时序）

  * fsdemo 每行单次 `sys_print`（原子行），避免被抢占时其它进程输出拆断日志行

  * QEMU HMP `sendkey` 不支持 `/`（静默丢弃）——交互注入用平铺名，路径验证走串口通道

  * `cmd()` 偶发注入丢失时自动重发一次；DURATION 20→35s 给 fsdemo 负载留余量

* QEMU 回归新增 v0.14 检查项：fsdemo 全流程、ls 目录类型标记、shell mkdir/rmdir

* 串口终端回归补 `mkdir /sd1` / `ls /sd1` / `run fsdemo` / `rmdir /sd1` 用例

## [v0.13] - 2026-08-29 · 用户栈守卫页与栈溢出检测

**Added**

* **用户栈守卫页（guard page）**：用户地址空间布局重构（mem.h）——
  每进程栈区改为 **8KB 槽** = \[守卫页 4KB（不映射） | 栈页 4KB（映射）]，
  栈从槽顶向下增长，下溢越过栈页底部即进入守卫页（未映射陷阱页）。

  * `USER_STACK_AREA_BASE=0x80010000`，槽按 pid 错开（`+ pid*0x2000`），
    `SHMEM_VBASE` 后移至 `0x80044000` 避免与栈区重叠

  * `spawn/spawn_at/exec` 只映射栈页，**守卫页不映射**（sched.c `user_stack_vbase`）

* **栈溢出检测**：`stack_guard_hit(fault)`（src/guard.c，纯逻辑可宿主单测）——
  页错误处理 `pf_handler` 先判定 fault 是否落在本进程用户栈守卫页，
  命中即打印 `[user] STACK OVERFLOW pid=.. @.. -> killed` 并终止该进程（内存安全演示）

* 演示应用 `src/apps/stackovf.c`：故意往本进程栈槽的守卫页写入 →
  触发页错误 → 内核识别为 STACK OVERFLOW 并隔离终止

**Engineering**

* 宿主单元测试 `tests/test_guard.c`（15 条断言：栈区外/pid0..2 槽内守卫页边界/
  跨槽边界/地址 0 与全 F 等）验证 `stack_guard_hit` 边界

* QEMU 回归新增 v0.13 检查项：initramfs 写入 stackovf / stackovf 启动 /
  栈溢出被检测（`STACK OVERFLOW pid=`）/ stackovf 被终止（kill + exited）

* 串口终端回归补 `run stackovf` 用例

* Makefile 新增 `stackovf` 应用构建与 `guard.o` 内核对象

* 初始化 Git 仓库：v0.12 基线提交 `ac80cc9`，v0.13 作为增量特性直接在主分支提交

## [v0.12] - 2026-08-29 · fork/exec 进程模型与 argv 参数传递

**Added**

* `sys_fork()`（SYS\_FORK=24）：复制当前进程——用户地址空间**深拷贝**（逐页分配新物理帧并拷贝内容），
  共享内存区（`SHMEM_VBASE`）保持共享；父进程返回子 pid，子进程从 fork 调用点继续（返回 0）

* `sys_exec(name, argc, argv)`（SYS\_EXEC=25）：镜像替换——加载 ELF 到新地址空间，释放旧地址空间，
  复用当前 pid 与内核栈，按 cdecl 在新用户栈布置 argv 块后切入新程序入口

* **pid 槽位重用**：`alloc_pid()` 扫描空闲 PCB 槽，进程退出后 pid 可复用
  （单调递增的 next\_pid 会在并发演示（fork/isol 等）耗尽 MAX\_PROCS 时无法再创建）

* 应用入口统一为 `app_main(int argc, char **argv)`：内核以 cdecl 进入
  （`[esp]=返回地址, [esp+4]=argc, [esp+8]=argv`），argv 数组与字符串布置在新用户栈顶

* 演示应用：

  * `forkdemo`：fork 父子分叉，双方把同一虚拟地址映射到不同物理页 → 双 `ISOLATED OK`

  * `args`：打印 argc 与每个 argv（argv\[0]=程序名）

* shell 新增 `exec <prog> [args...]` 命令：**经典 fork+exec+argv+wait 全链路**
  （fork 子进程 → 子进程 exec → 父进程 wait）

* PCB 新增 `fork_frames/fork_fcount`：fork 深拷贝出的物理帧，退出时统一回收

**Fixed**

* BUG-010：`sched_exec` 释放旧地址空间后才 `set_name`，而 name 指向旧用户栈 → 缺页

* BUG-011：`sys_exec` 切 CR3 后 `load_elf_file(name)` 才读 name（旧用户栈）→ 缺页；
  name 与 argv 须在切 CR3 前拷入内核缓冲

* BUG-012：argv 的 cdecl 栈布局顺序错位（argc/argv 槽、argv 指针槽），多次修正后正确

**Engineering**

* QEMU 回归新增 v0.12 检查项：fork 父子分叉/子进程返回 0/深拷贝隔离、exec 镜像替换、
  argv 参数传递（argc=4、argv\[1] 内容）、exec 退出码

* 串口终端回归补 `run forkdemo` 与 `exec args hello world` 用例

* Makefile 新增 `forkdemo`/`args` 应用构建与 initramfs 落盘

* 共享内存区布局常量移入 mem.h（fork 识别共享页跳过深拷贝）

## [v0.11] - 2026-08-29 · 每进程地址空间与物理内存隔离

**Added**

* 每进程独立地址空间 `mem.c/h`：`addr_space_create/destroy`（克隆内核共享 PDE + 清空用户半区）、
  `map_page_in`（映射到指定页目录）、`switch_page_dir`（写 CR3 自动刷 TLB）

* PCB 新增 `page_dir`：每个进程持有自己的页目录物理地址；idle/内核使用内核页目录（`page_dir=0`）

* 调度器上下文切换时同步切 CR3：`schedule()/sched_start()/sched_switch` 切到目标进程地址空间

* ELF 加载适配：`usermode_spawn_elf` 建独立地址空间 → 加载期间把 CR3 切到目标页目录直接写入
  段数据（不再临时映射进父进程页目录，避免覆盖父进程自身映射）

* 新系统调用 `sys_map_page(vaddr)`：用户进程在**自己的私有地址空间**申请物理页并映射

* 共享内存适配：`sys_shmem` 每次调用都重新映射共享物理帧进当前进程页目录（v0.11 起各进程页表不再共享）

* 隔离演示应用 `src/apps/isol.c`：两个并发实例把同一虚拟地址 `0x80050000` 映射到**不同物理页**，
  各自写入独立值并读回校验 → `ISOLATED OK`（物理内存隔离的铁证）

* PCB 新增 `map_frames/map_fcount`（用户经 sys\_map\_page 申请的物理页，退出时回收）

* PCB 新增 `name_buf[16]`：进程名拷入内核内存（父进程字符串位于其用户地址空间，子进程退出时 CR3 已切走，不能直接读）

**Fixed**

* BUG-009：引导期过早 `sti` 导致定时器抢占、shell 未注册即调度用户进程（见 bugs.md）

* GCC 14 构建可移植性：GCC 14 默认把 `-Wint-conversion`/`-Wincompatible-pointer-types`
  升级为编译错误——

  * `userprog.c` 52 处 `syscall3(SYS_PRINT, "字符串", ...)` 隐式指针→整数转换改为
    经 `sys_print()` 封装显式 `(uint32_t)` 窄化（与 user\_lib.h 语义一致）

  * `serial.h` 的 `serial_rx_hook_t` 回调签名由 `void (*)(char)` 修正为 `int (*)(char)`
    （匹配 `kb_feed_char` 真实签名）

  * 已用 GCC 14.2.0 与 GCC 13.3.0 双编译器验证 `make test` 全绿

**Engineering**

* QEMU 回归新增 v0.11 检查项：isol 映射私有页、`ISOLATED OK`、两个实例落到 ≥2 个不同物理页

* 串口终端回归 `tests/test_serial.sh` 补 `run isol` 用例

* Makefile 新增 `isol` 应用构建与 initramfs 落盘

## [v0.10] - 2026-08-29 · 串口终端：外部 agent 经 QEMU 交互

**Added**

* 串口接收通道 `serial.c/h`：IRQ4 中断处理、`serial_rx_ready/getc/set_rx_hook`，
  接收中断到达后把 FIFO/缓冲内所有可用字符取走转发

* 输入源统一：`kb_feed_char(c)` 抽出"注入一个已解析 ASCII 字符"的公共路径
  （键盘查表结果或串口字符共用同一行缓冲），支持退格/回车/可打印字符

* `kernel.c` 把串口接收钩子接到键盘行缓冲：`serial_set_rx_hook(kb_feed_char)`

* PIC 掩码放开 IRQ4（`0xEC`）：`qemu -serial stdio` 即成为可交互的串口终端

* 终端回归脚本 `tests/test_serial.sh`：以 FIFO 管道模拟"外部 agent 通道"，
  经串口发送命令并校验输出（help/ls/cat motd/run hello/run echo/run crash），
  与 qemu\_regression.sh（键盘 sendkey 路径）互补，验证"终端通道"

**Engineering**

* Makefile 新增 `test-serial` 目标并纳入 `test`（test-host + test-qemu + test-serial）

* `make run-serial` 可用 `-serial stdio` 直接交互，也可被外部 agent/工具驱动

## [v0.9] - 2026-08-29 · 可执行程序加载与交互式 Shell

**Added**

* ELF32 加载器 `elf.c/h`：解析程序头（PT\_LOAD）、按链接地址加载、bss 清零、
  `mapfn` 钩子逐页申请物理帧并映射；`elf_load_range` 预计算页对齐的加载区间

* 应用独立编译为 ELF（`src/apps/`），链接到固定地址（普通应用 `0x80040000`、shell `0x80030000`），
  整体内嵌进内核，启动时作为 **initramfs** 写入 ramdisk（motd + hello/echo/crash/shell）

* 内核启动时从文件系统加载**常驻 shell**（`usermode_spawn_elf("shell", SHELL_LINK, resident=1)`，
  帧不随退出回收）

* 交互式 shell 应用 `src/apps/shell.c`：命令 `help / ls / cat <file> / run <prog> / exit`

  * `run <prog>`：`sys_spawn_file` 把 ELF 应用加载到 app 槽 → `sys_wait` 等其退出并打印退出码

* 新系统调用：`SYS_READLINE(20)`（阻塞式读一行）/ `SYS_SPAWN_FILE(21)`（从文件加载 ELF 建进程）/
  `SYS_WAIT(22)`（等待子进程退出，返回退出码）

* 用户应用：

  * hello：打印 pid/ticks 后退出（演示"从文件系统加载程序"）

  * echo：阻塞式 `readline` 读一行并回显（演示用户态阻塞 I/O）

  * crash：ring3 写内核显存 0xB8000 → 页错误 → 内核隔离终止（内存保护演示）

* 键盘行缓冲 `kb.c`：行缓冲与字符环形缓冲解耦、退格处理、行完成回调 `kb_set_line_hook`；
  进程阻塞在 `sys_readline` 上（`BLOCK_KEYBOARD`），行就绪时由 `sched_wake_keyboard` 唤醒并拷入

* 调度器扩展：`BLOCK_WAIT`（等子进程退出，exit 时唤醒并携带退出码）、`sched_spawn_at`、
  PCB 新增 `own_frames/own_fcount/own_vbase`（从文件加载的 ELF 代码帧，退出自动回收）

* 宿主单元测试：`tests/test_kb.c`（290 条：行缓冲/退格/越界/多次取行）、
  `tests/test_elf.c`（36 条：段加载/绝对寻址/bss 清零/mapfn 钩子/畸形输入/加载区间计算）、
  `tests/test_heap.c` 补 `frame_alloc_run` 多页分配用例

**Fixed**

* BUG-007：ELF 首段含 ELF 头所在页（`-Ttext` 地址的前一页）时，硬编码映射区间拒绝映射 → 拷贝缺页；
  改用 `elf_load_range` 动态计算 PT\_LOAD 覆盖区间解决

**Engineering**

* Makefile 多 ELF 构建：`-Ttext` 固定地址 + `-e app_main` 指定 ELF 入口；
  `objcopy -I binary` 内嵌**完整 ELF 文件**（保留文件头供内核解析），区别于旧版 `objcopy -O binary`

* QEMU 回归升级为**交互式注入**：经 HMP monitor `sendkey` 注入键盘序列，
  端到端校验 shell 的 `help / ls / cat motd / run hello / run echo / run crash`

* 测试脚本同步基线：发送命令前记录日志行号，避免命中旧输出/漏掉同步写入

## [v0.8] - 2026-08-29 · 文件系统（内存盘 + 极简 mini-fs）

**Added**

* 块设备抽象 `blockdev.c/h`：以 4KB 块为单位的 read/write/ptr 接口，屏蔽后端差异；
  当前后端为内存盘（ramdisk，物理帧连续区，落在内核低 16MB 恒等映射区，可直接寻址）

* 极简文件系统 `fs.c/h`（类 Unix 磁盘布局）：

  * 块 0 超级块（magic "MINI"/总块数/inode 数）

  * 块 1 inode 位图、块 2 数据块位图、块 3 inode 表（64 个）、块 4.. 数据块

  * 只支持根目录、单级目录，文件名 <= 23 字符，直接块映射（单文件最大 12\*4KB=48KB）

* 文件操作：`fs_init`（格式化）/`fs_create`/`fs_lookup`/`fs_delete`/`fs_read`/`fs_write`/`fs_list`，
  目录缺块自动扩容，写时按需分配数据块（新块清零），跨块读写自动切块

* 系统调用 13\~19：`sys_fs_create/open/write/read/close/ls/delete`

  * 内核维护打开文件表 `fs_files[8]`（槽 0 保留），记录 inode/读写位置/模式（0 读 1 写）

  * 用户经固定槽位引用已打开文件，write/read 从当前位置推进（顺序 IO）

* 用户演示：两个新进程 procFSA/procFSB

  * procFSA：创建 hello.txt → 写入 8000 字节（跨块）→ 关闭 → 读回逐字节校验 → verify OK

  * procFSB：创建 alpha.txt/beta.txt → 写入 alpha.txt → 内核打印根目录列表（ls）→ 完成

* 宿主单元测试 `tests/test_fs.c`（1182 条断言：格式化/创建/重名拒绝/读写回读/
  跨块边界覆写/随机偏移抽查/删除后位图回收/inode 耗尽/目录扩容）

**Engineering**

* blockdev + fs 抽成纯逻辑模块（只依赖内存缓冲，不依赖内核/调度/硬件），宿主单测覆盖

* QEMU 回归新增 v0.8 检查项：内存盘初始化/进程 spawn/文件创建/写模式打开/跨块写入/
  读模式打开/读回校验通过/多文件创建/ls 列出/演示完成

* 用户程序新增 `.text.fs` 段（procFSA/procFSB 入口）；Makefile 加入 blockdev.o/fs.o

## [v0.7] - 2026-08-29 · IPC：有界消息队列（生产者-消费者）

**Added**

* 有界消息队列 `msg.c/h`：环形缓冲 + 双 FIFO 等待队列（生产者/消费者）

* 发送阻塞时**暂存消息**：`msg_send_try` 返回"应阻塞"并把 {pid, 消息} 入生产者队列，
  消费者取走后由 `msg_recv_wake` 把暂存消息搬入缓冲并唤醒（send 由内核代发，视为成功）

* 接收交棒语义：`msg_send_wake` 直接把刚入队的消息交付给等待消费者
  （消费者 recv 直接返回该消息，缓冲不滞留），保证消息恰好送达一次

* 调度器扩展：PCB 新增 `block_reason=BLOCK_MSG` 与 `block_arg` 字段；
  `sched_wake_with(pid, eax)` 支持唤醒时指定系统调用返回值（recv 返回消息值）

* 系统调用：`sys_msg_create(id, capacity)` / `sys_msg_send(id, value)` / `sys_msg_recv(id)`

* 用户演示：两个新进程 procMsgP/procMsgC

  * 消费者先建，立即在空缓冲上 recv 阻塞（recv-block）

  * 生产者快产慢消，塞满缓冲后 send 阻塞，由消费者取走唤醒（send-block）

  * 20 条消息 0..19 按序恰好一次送达，双方退出并被回收

* 宿主单元测试 `tests/test_msg.c`（117 条断言：环形回绕、双队列 FIFO、暂存/交棒、边界）

**Engineering**

* 消息队列抽成纯逻辑模块（无调度/硬件依赖），宿主单测覆盖

* QEMU 回归新增 v0.7 检查项：队列创建/消费者阻塞/生产者阻塞/生产者唤醒/收发完成

* 用户程序新增 `.text.msg` 段（procMsgP/procMsgC 入口）

## [v0.6] - 2026-08-29 · IPC 与同步（信号量 + 共享内存）

**Added**

* 信号量 `sem.c/h`：计数 + FIFO 等待队列；`sem_wait_try`（占用/应阻塞标记）、`sem_signal_wake`（唤醒队首/归还资源）

* 调度器扩展：`sched_block`（按原因阻塞当前进程）、`sched_wake`（唤醒并入就绪队列）、PCB 新增 `block_reason` 字段

* 系统调用：`sys_sem_create(id, init)` / `sys_sem_wait(id)` / `sys_sem_signal(id)` / `sys_shmem(slot)`

* 共享内存页：内核预映射共享物理帧到固定虚拟地址（所有进程共享页表，天然互通）

* 用户演示：两个新进程 procSemA/procSemB

  * **rendezvous 会合**：两个信号量双向等待，展示阻塞/唤醒（"arrived → rendezvous done"）

  * **互斥共享计数**：持锁后 sleep 强制对端在 `wait` 上阻塞，10 次自增最终恰为 10，无竞争丢失

* 宿主单元测试 `tests/test_sem.c`（68 条断言：计数增减、FIFO 顺序、满队列、资源守恒）

**Fixed**

* BUG-004：信号量等待者被定时器误唤醒（`sched_tick` 原只按 `wakeup_tick` 判阻塞）

* BUG-005：阻塞系统调用唤醒后 eax 返回值错误

**Engineering**

* 信号量抽成纯逻辑模块（无调度/硬件依赖），宿主单测覆盖

* QEMU 回归新增 v0.6 检查项：信号量创建/等待阻塞/唤醒/共享内存/rendezvous/互斥自增

* 内存布局扩展：共享页区 0x80020000（避开用户栈区）

## [v0.5] - 2026-08-28 · 抢占式多任务与进程调度

**Added**

* 进程模型：PCB（pid、READY/RUNNING/BLOCKED/ZOMBIE/FREE 状态机）、进程表

* 抢占式轮转调度：PIT 100Hz 心跳驱动 `sched_tick`，多进程轮流运行

* 调度原语：`yield`（主动让出）、`sleep`（按 tick 阻塞并按时唤醒）、`exit`/`kill`（退出 + 僵尸回收）

* 内核 idle 进程（PID 0）：无就绪进程时 `sti; hlt` 兜底，负责状态刷新与键盘回显

* 多进程共享同一份用户代码页，各自独立内核栈 + 用户栈

* 宿主单元测试 `tests/test_sched.c`（调度队列策略，84 条断言）

**Engineering**

* 源码/产物分离：`src/` + `build/`；`make clean` 一键清理

* `sched_policy.c` 抽成纯逻辑模块，支持宿主单测

* QEMU 回归新增 idle 心跳、定时器心跳校验项

**Fixed**

* BUG-001（早期遗留确认）：上下文切换改为 `jmp resume_point` 恢复现场

* BUG-002：idle 空队列返回路径不再 `cli; hlt`，修复系统挂死

## [v0.4] - 用户态 ring3 与系统调用

**Added**

* 重建 GDT（kernel/user 代码段 + 数据段 + TSS），`ltr` 加载 TSS

* `int 0x80` 系统调用门（DPL=3）：`exit / print / get_ticks / sleep / yield / get_pid`

* ring3 用户程序加载与运行（共享代码页 0x80000000）

* 内存保护演示：用户态写内核地址 0xB8000 → 页错误 → 进程终止

* 键盘回显主循环

## [v0.3] - 内存管理

**Added**

* 物理页帧分配器（4KB 粒度）

* 分页开启 + 页表管理（`map_page`）

* 内核堆 `kmalloc/kfree`（首适应）

* 懒分配：缺页按需映射，`pf_handler` 支持懒分配区恢复

* 内存自检与状态行（free / heap / lazy）

## [v0.2] - C 内核地基

**Added**

* multiboot 引导（QEMU `-kernel` 直接加载）

* GDT/IDT、8259 PIC 重映射、CPU 异常处理

* PIT 定时器（100Hz 心跳）、PS/2 键盘、VGA 文本 + 串口输出

* 交互式回显（可键入内容，回车换行）

## [v0.1] - 引导与保护模式

**Added**

* 软盘引导扇区（`v1-floppy/`）

* 实模式 → 保护模式切换

* VGA 打印 "Hello Micro-OS!"
