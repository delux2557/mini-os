# mini-os v2-c-kernel shell（用户态交互）专项深审报告

> **来源**：代码审查专家（本轮专项审计）
> **审计对象**：`v2-c-kernel/src/app/shell.c` + `shell_heredoc.h`（纯逻辑，配宿主单测 test_heredoc）
> **方法**：逐行读码 + 命令解析/heredoc 写文件/进程编排推演。
> **边界**：纯静态读码，未动态运行。

---

## 0. 结论

shell 是 **演示态交互用户的命令解释器**：readline 阻塞读行 + tokenize 分发 + 从 FS 加载应用 + 自检
汇总。命令解析/heredoc 终结判定为**纯逻辑且可宿主单测**，历次解析 bug（PR #25 DELIM 长度误用等）
已封堵。无 P1，登记 `OBS-SH-x`（4×P3）。

---

## 1. 对象与边界

- 定位：用户程序（链到 0x80000000），非内核模块；命令经 `sys_readline` 阻塞获取。
- 方法：静态读码；未动态运行。

## 2. 架构正向核实

- **三套解析函数分工**：`split_cmd`（取命令）、`split_arg`（取第一个参数）、`tokenize`（就地空格切
  tokens，空格→\0）——分别供命令分发、路径命令、多参数命令（ccrun 用 tokenize[4]）复用，边界均
  `< ARG_MAX`/`max` 钳制。
- **heredoc DELIM 判定为独立纯逻辑**（[shell_heredoc.h:15-24](file:///workspace/mini-os/v2-c-kernel/src/app/shell_heredoc.h#L15-L24)）：
  `wf_delim_hit` 先去头尾空白、再比长度（用调用方保存的 `delim_len`，而非复用 j）、逐字符比对。
  该长度为"DELIM 比对基准"，根治了 PR #25 用 path 长度误判致 heredoc 永不终结的 bug；且被
  `test_heredoc.c` 宿主单测覆盖，防"bug 直达 CI"。
- **writefile heredoc/单行双模式**：heredoc 遇 DELIM 收尾（空行写 `\n` 保留结构）；单行模式 `content`
  直接就地指向 args 中段（零拷贝）。关闭 fd 路径完整。
- **自检汇总原子化（F-4）**：`nl_reset/nl_s/nl_u/nl_end` 把 `[selftest] PASS/FAIL` 先拼进 `nl_buf`
  再单次 `sys_print`，避免被内核异步打印撕裂（BUG-042 域封堵）。

## 3. 发现项（`OBS-SH-*`）

### OBS-SH-1【P3】命令分发为 15 项 if/else 线性匹配（非哈希/trie）
`cmd` 用 `user_strcmp` 逐一比较 15 个命令（[shell.c:549-563](file:///workspace/mini-os/v2-c-kernel/src/app/shell.c#L549-L563)）。
交互态频率低、可读性优先，线性匹配合理；P3 观察：若命令增至几十个可改静态字符串表 + 二分，当前不必要。

### OBS-SH-2【P3】`cmd_writefile` 单行内容不做长度显式校验（依赖 ARG_MAX 隐含界）
单行模式 `content = &args[i]`，长度受外层 `line[CMD_MAX=128]`/`args` 隐含钳制，未在 writefile 内再显式校验
（[shell.c:481-491](file:///workspace/mini-os/v2-c-kernel/src/app/shell.c#L481-L491)）。当前由 readline 边界保证，
但 writefile 内部语义上"内容长度"未显式声明。P3：可加 `user_strlen(content) < CMD_MAX` 早退提示。

### OBS-SH-3【P3】PATH 解析与 DELIM/PATH 长度上限分别使用不同硬编码（31/63）
heredoc 的 DELIM 上限 31、PATH 上限 63、单行 PATH 上限 63（[shell.c:433](file:///workspace/mini-os/v2-c-kernel/src/app/shell.c#L433)/[shell.c:438](file:///workspace/mini-os/v2-c-kernel/src/app/shell.c#L438)/[shell.c:478](file:///workspace/mini-os/v2-c-kernel/src/app/shell.c#L478)），
与 `path[64]`/`delim[32]` 缓冲一致但魔法数字分散。P3：建议收敛为 `DELIM_MAX`/`PATH_MAX` 宏 + 编译期断言。

### OBS-SH-4【P3】`tokenize` 返回 token 数，但命令解析不统一使用它
`cmd_ccrun` 用 `tokenize`，其余命令用 `split_cmd/split_arg` 双函数（各自再扫一遍）。两种解析风格并存，
未来新增多参数命令需选型。P3：可统一到 `tokenize`（shell 内发起命令至多 4 参数，够用），减少重复扫描。

## 4. 与历史 bug 对照

| 历史 | 现况 | 状态 |
|---|---|---|
| PR #25 heredoc DELIM 永不终结 | wf_delim_hit 用 delim_len；宿主单测 | 已封堵 |
| BUG-042 selftest 汇总行撕裂 | nl_* 单次 flush | 已封堵 |

## 5. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| OBS-SH-1 | P3 | 观察 | 命令表化（当前线性可接受） |
| OBS-SH-2 | P3 | 防御 | writefile 内容长度显式校验 |
| OBS-SH-3 | P3 | 可读性 | DELIM/PATH 上限收敛为宏 |
| OBS-SH-4 | P3 | 一致性 | 统一 tokenize 解析 |

*注：全项静态推演；当前命令集 15 项、演示交互流量下均正确，观察项为可读/演进加固。*