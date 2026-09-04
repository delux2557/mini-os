# mini-os 文档总入口

> 本文档是 `docs/` 的导航与治理说明。目标读者：**dev / ops / 评审 agent 与任何要改这份代码库的协作者**。
> 读文档先读这里：知道有什么、哪份是活的、改代码时该同步哪份。

## 文档定位

mini-os 的开发文档遵循三条铁律：

1. **版本历史只进 `changelog.md`**；README / roadmap 一律链接不复制，杜绝多处漂移。
2. **活文档只描述"当前真值"**（当前架构、当前 bug 全集、当前测试矩阵）；时点产物（评审、审计、路线图）归档进 `history/`，不占活文档区。
3. **改代码必须同步文档**（见下"维护规则"）——文档与代码一起过 PR 门禁。

## 文档清单

| 文档 | 内容 | 生命周期 | 何时更新 |
|---|---|---|---|
| [design.md](explanation/design.md) | 架构设计与开发思路（22 章，覆盖至 v1.4.x + 并发模型不变量） | 活文档 | 新子系统 / 新架构决策 → 加章（带版本戳） |
| [roadmap.md](explanation/roadmap.md) | 当前阶段判断 + 未完成主线 + 支线 + 红线 | 活文档 | 规划变更时；已完成项迁出到 `history/` |
| [changelog.md](reference/changelog.md) | 版本变更日志（唯一版本历史源，最新在顶） | 活文档 | 每个版本发布时；**README/roadmap 不复制其内容** |
| [bugs.md](reference/bugs.md) | Bug 记录（**编号被代码注释引用，严禁改号/删号**） | 活文档 | 新 BUG → 新增条目 + changelog Fixed 段（两处都要） |
| [security.md](reference/security.md) | **安全威胁模型显式化**（信任边界 / 态度取舍 / 已知观察对账 / 新件安全自查清单） | 活文档 | 安全语义变更时；新 BUG/OBS 需在此补"为何接受"摘要 |
| [tcp-session-proto.md](tcp-session-proto.md) | 虚拟 TCP 会话协议头规范（netif Step 4, 1/3） | 契约（定稿） | 协议变更时（动码前先改文档） |
| [tcp-thin-api.md](tcp-thin-api.md) | 虚拟 TCP 薄包装 API 契约表（netif Step 4, 2/3） | 契约（定稿） | API 变更时（动码前先改文档） |
| [tcp-mtu-fail.md](tcp-mtu-fail.md) | MTU/大包失败路径规范（netif Step 4, 3/3） | 契约（定稿） | 失败路径变更时 |
| [history/external-reviews/](history/external-reviews/README.md) | 外部评审报告 + 缺陷对账索引（F-x/OBS-y ↔ bugs.md ↔ commit） | 只读归档 | 新评审入库（时点产物不占活文档区） |
| [history/](history/) | 已归档时点产物（路线图、里程碑、外部评审等，只读） | 只读归档 | 内容全落地后归档入库 |
| [screenshots/](screenshots/) | 各版本运行截图 | 资源 | 随版本补充 |

> 命名约定：时点产物（评审/审计/路线图）一律 `history/` 归档，文件名带对象版本或 commit sha 标识。
> external-reviews 子目录细则：审计对象为**单点 commit** 的报告带对象 sha 后缀（如 `_6ac70e4`）；
> 审计对象为**代码目录/多文件**（无单点 sha 可指）的深审报告省略后缀，其对象在报告头"审计对象"行标注。

## 维护规则（摘要）

1. **新 BUG**：`bugs.md` 新增条目（沿用既有唯一编号体系，不重排）+ `changelog.md` 当前版本 Fixed 段。两处都要。
2. **新子系统 / 新架构决策**：`design.md` 增章，带 v0.x/v1.x 版本戳，延续既有风格。
3. **规划变更**：只有"未开始的计划"进 `roadmap.md`；一旦落地，迁入 changelog 并把 roadmap 对应段删除或标注。
4. **契约文档（tcp-\*）**：动码前定稿；实现与测试以文件为准，改契约先改文档。
5. **测试层清单（唯一事实源）**：层集合只在 `v2-c-kernel/Makefile` 的 `TEST_LAYERS` 维护，并划分为 `TEST_LAYERS_FAST`（秒级层，同一 runner 顺序跑）与 `TEST_LAYERS_HEAVY`（其余慢层，进并行矩阵），二者相离并入覆盖全集（FAST ∪ HEAVY == TEST_LAYERS）。`make test` 聚合、`.github/workflows/layers.yml` 的 fast job（消费 `test-fast`）与 layer 矩阵（消费 `test-layers-heavy`）均由它生成。**不得在 `.github/workflows/*.yml` 中手抄层名清单**——它们是生成物/消费方（曾因多处手抄漏加 test-stack）。加层/划层步骤见规则 7。
6. **docs PR 也是 PR**：走同一门禁；纯文档变更在 PR 描述注明，便于 reviewer 聚焦。
7. **新增一个测试层（checklist）**：
   - ① 定义 `test-<name>` 目标（`v2-c-kernel/Makefile`）；
   - ② 往该文件 `TEST_LAYERS` 加该名字，并归入 `TEST_LAYERS_FAST`（整层 <10s，同一 runner 顺序跑）或 `TEST_LAYERS_HEAVY`（进并行矩阵）——二者必须相离并入覆盖全集；
   - ③ 确认是否需进 regression-rr / 特殊 job（`test-tr-stable`/`test-repro`/`test-tcp-attack` 是有意例外、不进主链，Makefile 注释已写明）；
   - ④ 在 CI 上看它出现在 fast job（FAST）还是 layer 矩阵（HEAVY）。
   - **移动标准**：某层从秒级变慢（或反之）时，只改 `TEST_LAYERS_FAST`/`TEST_LAYERS_HEAVY` 里的名字归属，CI 自动跟着走；以"整层 <10s"为 FAST 划分依据。
   - 若某层被有意排除在并行矩阵或主链之外，必须在 Makefile 注释与本文档同时说明理由。
8. **CI 工具链清单（唯一事实源）**：依赖构建/跑测试的 job 所需 apt 包只在 `v2-c-kernel/Makefile` 的 `CI_RUNNER_PKGS` / `CI_RUNNER_PKGS_EXTRA` 维护，安装步骤一律写 `sudo apt-get install -y $(make -s -C v2-c-kernel ci-pkgs)`（`ci-pkgs` 输出 = 基础 ⊕ EXTRA）。**不得在 `.github/workflows/*.yml` 中手抄包名**（曾因 layers 手抄缺 `qemu-user` 靠环境侥幸，更换 runner 镜像即炸）。加包步骤：
   - ① 多数 job 需要的包 → 追加到 `CI_RUNNER_PKGS`；
   - ② 个别 job 特有的包 → 追加到 `CI_RUNNER_PKGS_EXTRA`，并在旁边注释写明用途；
   - ③ 确属"无需工具链"的纯静态 job（如 ci.yml 的 lint：只跑 grep/脚本）在 workflow 注释中显式声明为白名单例外，不消费该清单；**禁止散落的 `apt-get install` 追加行**。
9. **门禁观测打点（插曲 1/2，Commit 2/3）**：三个测试脚本（`test_serial.sh`/`test_socket.sh`/`qemu_regression.sh`）在各自等断函数（wait_for/wait_after）每断言恰打一行，落盘 `$BUILD/assert_timing_<script>.tsv`（serial/socket/qemu 各一文件，分文件防 `make test` 串行同 BUILD 互覆盖），字段=`断言名 \t 耗时ms \t ok|timeout`；`make clean` 对 `assert_timing_*.tsv` 做 stash→放回（只清构建、留观测台账）。随 CI artifact 上传，经 `tests/tr2sqlite.py --assert-timing` 导入 sqlite，`tests/baseline_check.py --asserts` 做跨轮 P50/P95 基线。
   - **断言名即 RR 基线 key**：改名即新基线——三脚本的断言名保持稳定，勿随日志文案改动而随意 rename（先确认确实换断言语义再改）。
   - **GNU date 依赖**：打点用 `date +%s%3N`（GNU date）。CI=ubuntu 无碍；本地 mac 的 BSD date 不支持 `%3N`，脚本内已 `|| echo 0` 兜底（耗时记 0、不崩不误判）。
   - **耗时是 250ms 轮询粒度**（0.25s sleep），非真毫秒；仅作跨轮 P50/P95 趋势信号，P50/P95 与 timeout 判定不受粒度影响。
   - **MD047**：所有 *.md 文件末尾必须保留换行（门下 lint 曾 5 次回退）。

## 已知待办（治理）

| 项 | 状态 |
|---|---|
| changelog 版本顺序乱序（v1.4.4→1.4.7 顶部后接 v1.4.3） | ✅ 已修（PR #49，3e54458） |
| design.md 落后 ~10 版（停 v0.27/v0.28，缺 v0.29~v1.x） | ✅ 已补写（20~22 章：加固收口 / RR 确定性 / 并发不变量，见 PR #51） |
| docs 目录四象限重组（explanation/ / reference/ 分层） | ✅ 已落地（PR #52：design/roadmap → explanation/，changelog/bugs → reference/；契约与 history/ 留根，不设空 guides/） |
| external-reviews 命名统一（部分缺审计对象 sha） | ✅ 已闭环（P2 迁移 history/，规则明文化：sha 可考才带后缀） |

---

*本文档随 P0 文档治理建立；结构重组（P2）落地于 PR #52，活文档已按 explanation/（design、roadmap）与 reference/（changelog、bugs）分层，契约文档 tcp-\* 与归档 history/、资源 screenshots/ 留在 docs 根。*
