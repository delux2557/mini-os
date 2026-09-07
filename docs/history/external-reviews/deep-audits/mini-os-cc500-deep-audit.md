# mini-os v2-c-kernel cc500 自举编译器专项深审报告

> **来源**：代码审查专家（本轮专项审计）
> **审计对象**：`v2-c-kernel/tools/cc500/cc500.c`（1013 行）+ `host_crt.c`（35 行）
> **性质**：继承自 Edmund Grimley Evans 的 tcc 前身、由 mini-os 移植为内核/guest 自举工具链的
> **受限 C 子集编译器**（来源 tcc「A self-compiling compiler」）。
> **方法**：逐行通读 + tokenizer/递归下降/sym 表 trie/代码生成器逐路径推演 + 与 BUG-026/032/036/039/040/041/048/049 对照。

---

## 0. 结论

cc500 是**极窄但自洽**的 C 子集递归下降编译器：单个 1013 行文件完成词法/语法/符号表/代码生成/ELF
拼装，且通过内核内**自举不动点校验**（P1==P2 逐字节）证明自身正确性——这个"编译器正确性由自举闭环保证"的
思路是整份工程最亮眼的点之一。历次 8 个 cc500 bug（未闭合字符串/注释/形参 EOF 死循环、混合字面量算错、
未定义符号废产物、关系运算残缺、argv 丢参）**全部封堵且有版本注解**。无 P1 缺陷，登记 `OBS-CC-x`
观察项（1×P2 + 4×P3）。

---

## 1. 对象与边界

- 环境：guest 内以 `SYS_BRK` + `SYS_FS_*` 支撑 malloc/文件 IO；唯一机器码 stub `syscall3`；
  输出一次性整写 `/out.elf`。自举验证：gcc 版跑自身 → P1，再跑 P1 编译自身 → P2，`ccboot` 比对
  P1==P2 字节。
- 方法：静态读码 + 代码生成字节推演；**未在 guest 内运行 P1/P2**，结论为架构/正确性评估。

## 2. 架构正向核实

- **单栈全局代码生成**：`stack_pos` / `codepos` 全局 + `be_push/be_pop` 维护 esp 栈深，递归下降
  各 `*_expr` 按优先级链（primary→postfix→additive→shift→relational→equality→bitwise and/or→assign）
  层层下降，代码简洁且与 x86 ABI 一一对应。
- **符号表 trie 锚点统一**：全表按"名字 NUL 下标 + stride 6"寻址（`t+1=class, t+2=value`），
  `sym_lookup/sym_declare/sym_define_global/be_finish` 遍历锚点一致，未发现索引错位。
- **ELF/机器码手工拼装**：`be_start` 直接 emit 16×4 字节 ELF32 头 + 程序头 + 入口 stub + syscall3 stub，
  `p_filesz/memsz` 由 `be_finish` 回填——完全免依赖、可被内核加载器解析。
- **历次 bug 封堵闭环**（重点核对，均成立）：
  - `get_token` 字符/字符串/块注释读取**全部带 EOF 守卫**（`nextc == 0-1`，见 [cc500.c:110-114](file:///workspace/mini-os/v2-c-kernel/tools/cc500/cc500.c#L110-L114)）→ BUG-026/039/048 死循环域封堵；
  - `primary_expr` 数字字面量逐字符 `'0'..'9'` 校验（[cc500.c:420-424](file:///workspace/mini-os/v2-c-kernel/tools/cc500/cc500.c#L418-L425)）→ BUG-049 封堵；
  - `be_finish` 遍历符号表发现残留 `'U'`（undefined）即报错（[cc500.c:371-385](file:///workspace/mini-os/v2-c-kernel/tools/cc500/cc500.c#L371-L385)）→ BUG-040/F-2 封堵；
  - 关系四则 `< >`（[cc500.c:607-637](file:///workspace/mini-os/v2-c-kernel/tools/cc500/cc500.c#L607-L637)）→ BUG-041/F-1 封堵；
  - 入口 stub 编组 argv/argc（[cc500.c:326-332](file:///workspace/mini-os/v2-c-kernel/tools/cc500/cc500.c#L324-L332)）→ BUG-032 封堵。

## 3. 发现项（`OBS-CC-*`）

### OBS-CC-1【P2】递归下降无深度护栏，深表达式可耗尽内核栈
`expression()` → `postfix_expr()` → 相互递归无深度上限；`statement()` 的块嵌套、`if/while` 重入
均直接递归。若输入为极端嵌套（如 `(((((...))))`、长链 `a=b=c=...`），**内核/guest 栈（KSTACK_SIZE）
可被耗尽**——内核侧表现为栈溢出（被 kill，隔离），但属可被「任意输入注入的崩溃引信」，与防线哲学
（拒绝畸形输入而非被击穿）相悖。建议：`statement/expression` 顶部加深度计数 + 超过上界即 `error()`。

### OBS-CC-2【P3】`my_realloc`/`emit`/`sym_declare` 的 realloc 均拷贝 `[0, oldlen-1]`，但无「新长度≥旧长度」显式断言
`takechar`/`emit`/`sym_declare` 传入 `x=(i+10)<<1` / `(codepos+n)<<1` / `(t+10)<<1` 均 ≥ 旧值，
实际安全。但 realloc 依赖调用方保证 `newlen≥oldlen` 这一隐式契约，无断言。建议加
`mysize` 校验避免未来误用（当前为隐式契约，P3 hygiene）。

### OBS-CC-3【P3】C 子集单栈全局方案限制可表达性，且已在多处用位运算非短路
`peek/accept` 判断用 `&`/`|`（[cc500.c:157-171](file:///workspace/mini-os/v2-c-kernel/tools/cc500/cc500.c#L156-L172)）——对 -1(EOF) 值参与字符区间比较不越界，但**无短路意味着下一分支也会执行**；
且 `stack_pos/codepos/type` 全为函数外全局，不支持重入（中断/多线程），符合单线程内核定位，但应在
文档中声明「cc500 为单线程、无递归护栏」的设计边界，避免误当通用编译器使用。

### OBS-CC-4【P3】部分失败路径 fs slot 未归还（资源记账域）
`open_input` / `setup_output` 在部分 `return -1` 分支后若已有 slot 打开未走 `SYS_FS_CLOSE`，
则占用全局 fs 槽（fs 槽全局共享，正是 BUG-031 同域）。当前失败前 slot 少、且多数失败发生在 open 前，
实害小；建议统一到「fail 即 close」路径并与 BUG-031 的 per-process fd 归属对齐。

### OBS-CC-5【P3】`open_input` 32KB 输入上限 + `in_data` 一次性整读
源文件 >32KB 才截断告警（[cc500.c:946-951](file:///workspace/mini-os/v2-c-kernel/tools/cc500/cc500.c#L946-L953)），
对自举（自身 ~19KB）够用。若未来编译更大源码需分段处理，属已知限制。建议文档化「32KB 输入上限」。

## 4. 与历史 bug 对照（防回归锚点）

| 历史 | 现况 | 状态 |
|---|---|---|
| BUG-026 形参 EOF 死循环 | program/形参列表 EOF 守卫 | 已封堵 |
| BUG-032 argv 丢参 | 入口 stub 编组 argc/argv | 已封堵 |
| BUG-036 GCC14 permerror | Makefile `-w` 豁免（记录） | 已封堵（工具链豁免策略债） |
| BUG-039 未闭合字符串越界 | 双守卫 + NUL 防护 | 已封堵 |
| BUG-040 未定义符号废产物 | be_finish 反馈报错 | 已封堵 |
| BUG-041 关系运算残缺 | 补齐 < / > / >= | 已封堵 |
| BUG-048 未闭合注释死循环 | EOF 守卫 + 状态机 | 已封堵 |
| BUG-049 混合字面量算错 | 逐字符数字校验 | 已封堵 |

## 5. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| OBS-CC-1 | P2 | 健壮性 | 递归下降加深度护栏，拒绝深嵌套注入 |
| OBS-CC-2 | P3 | hygiene | realloc newlen≥oldlen 断言 |
| OBS-CC-3 | P3 | 文档 | 声明子集/单线程/无递归护栏边界 |
| OBS-CC-4 | P3 | 资源 | 失败路径统一归还 fs slot |
| OBS-CC-5 | P3 | 文档 | 32KB 输入上限显式声明 |

*注：OBS-CC-1 为静态推演的崩溃引信（深嵌套注入耗尽栈），未做动态复现；当前内核栈大小下需极端嵌套，
列 P2 验证性加固。编译器本身自举闭环正确性不受影响。*