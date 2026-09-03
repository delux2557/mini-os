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
| [design.md](design.md) | 架构设计与开发思路（当前 19 章，落后于代码 ~10 版，待补） | 活文档 | 新子系统 / 新架构决策 → 加章（带版本戳） |
| [roadmap.md](roadmap.md) | 当前阶段判断 + 未完成主线 + 支线 + 红线 | 活文档 | 规划变更时；已完成项迁出到 `history/` |
| [changelog.md](changelog.md) | 版本变更日志（唯一版本历史源，最新在顶） | 活文档 | 每个版本发布时；**README/roadmap 不复制其内容** |
| [bugs.md](bugs.md) | Bug 记录（**编号被代码注释引用，严禁改号/删号**） | 活文档 | 新 BUG → 新增条目 + changelog Fixed 段（两处都要） |
| [tcp-session-proto.md](tcp-session-proto.md) | 虚拟 TCP 会话协议头规范（netif Step 4, 1/3） | 契约（定稿） | 协议变更时（动码前先改文档） |
| [tcp-thin-api.md](tcp-thin-api.md) | 虚拟 TCP 薄包装 API 契约表（netif Step 4, 2/3） | 契约（定稿） | API 变更时（动码前先改文档） |
| [tcp-mtu-fail.md](tcp-mtu-fail.md) | MTU/大包失败路径规范（netif Step 4, 3/3） | 契约（定稿） | 失败路径变更时 |
| [external-reviews/](external-reviews/README.md) | 外部评审报告 + 缺陷对账索引（F-x/OBS-y ↔ bugs.md ↔ commit） | 时点产物 | 新评审入库；活文档区不放时点快照 |
| [history/](history/) | 已归档时点产物（路线图等，只读） | 只读归档 | 内容全落地后归档入库 |
| [screenshots/](screenshots/) | 各版本运行截图 | 资源 | 随版本补充 |

> 命名约定：时点产物（评审/审计/路线图）一律 `history/` 归档，文件名带对象版本或 commit sha 标识。

## 维护规则（摘要）

1. **新 BUG**：`bugs.md` 新增条目（沿用既有唯一编号体系，不重排）+ `changelog.md` 当前版本 Fixed 段。两处都要。
2. **新子系统 / 新架构决策**：`design.md` 增章，带 v0.x/v1.x 版本戳，延续既有风格。
3. **规划变更**：只有"未开始的计划"进 `roadmap.md`；一旦落地，迁入 changelog 并把 roadmap 对应段删除或标注。
4. **契约文档（tcp-\*）**：动码前定稿；实现与测试以文件为准，改契约先改文档。
5. **测试目标增减**：更新测试矩阵说明，README / ci.yml 注释引用之，消灭"五层/九层"式口径漂移。
6. **docs PR 也是 PR**：走同一门禁；纯文档变更在 PR 描述注明，便于 reviewer 聚焦。

## 已知待办（治理）

| 项 | 状态 |
|---|---|
| changelog 版本顺序乱序（v1.4.4→1.4.7 顶部后接 v1.4.3） | 待 P2 修复（趁文件未更长） |
| design.md 落后 ~10 版（停 v0.27/v0.28，缺 v0.29~v1.x） | 待 P2 补写（可与并发模型不变量一节合并） |
| docs 目录四象限重组（explanation/reference/guides 分层） | 待 P2 |
| external-reviews 命名统一（部分缺审计对象 sha） | 待 P2 |

---

*本文档随 P0 文档治理建立；结构重组（P2）完成后本清单同步更新归属路径。*
