# cc500 自举编译器

`cc500.c` 是 [Edmund GRIMLEY EVANS](http://homepage.ntlworld.com/edmund.grimley-evans/cc500/)
的 cc500（Copyright © 2006）的**移植 / 衍生** C 子集编译器。本仓库内按 **GPL-2.0-or-later**
提供，许可证全文见本目录 [`LICENSE`](LICENSE)。

## 在本项目中的角色
mini-os 的自举工具链（v0.27 引入）：guest 内用 cc500 把 C 子集源码编译为 ELF 并运行，
支撑"写-编-跑 / 编译器自编译"闭环（shell `ccrun` / `ccboot`）。

**教学对照定位（2026-09 起）**：与自研 MIT 编译器 minicc（`tools/minicc/`）并列，作为
"一个编译器怎么长大"的活教材——cc500 以 ~1100 行保持极简单遍架构，特性按低成本里程碑
逐个追加，每个里程碑都附带「旧语法产物字节零变化 + 自举 P1==P2 不动点」的回归证明。

## 当前语言快照（教学里程碑后）

- 类型：`int` / `char`（`char*` 声明中 `*` 仅被语法跳过，无真指针类型语义）
- 字面量：十进制整数、字符 `'x'`、字符串（含 `\xNN \n \t` 转义，call/pop 内联取址）
- 运算符：`+ - * / %`、`<< >>`、`< <= > >=`、`== !=`、`& | ^`、`~ !` 一元、`=` 赋值
  （无 `&& ||` 短路、无 `+=`、无 `++/--`）
- 语句：`{}`、声明（含初始化）、`if/else`、`while`、**`for` / `do-while`（M1）**、`return`
- 函数：多参、递归、前向引用（U/D 链式回填）、间接 `call *%eax`

## 教学里程碑（"编译器长大"路线图）

| 里程碑 | 新增语法 | 要点 | commit |
| --- | --- | --- | --- |
| M1 | `for` / `do-while` | 无 AST 单遍下 for 的 step 后置 = 双跳摆渡布局（`jmp L_body` 跳过 step 区、body 尾 `jmp L_step`）；step 原地发射无重定位 | `feat(cc500)` M1 |
| M2 | 一元 `- ! ~`、`^` | unary 层插 postfix/additive 之间，"先试前缀失败回退"消歧同一 token；operator 词法精确化（原 `=!` 被宽集合吞并） | `feat(cc500)` M2 |
| M3 | `* / %` | multiplicative 层；imul/idiv 路径与 minicc 同编码；自举输入上限 32KB→64KB | `feat(cc500)` M3 |
| 候选 | `&& \|\|` 短路、`+=`、`++/--` | 需引入表达式内跳转/左值保持等新机制，风险较高，独立评估 | — |

**纪律（每里程碑强制）**：
1. 新特性只加**新增分支**，不改任何既有 emit 路径 → cc500 自身源码不使用新语法，
   自举不动点 P1==P2 逐字节保持；
2. 不支持的语法仍走 `error()`，绝不静默产出坏码；
3. 每特性三同步：`tests/test_cc500.sh` 宿主编译断言 + guest 运行语义断言 + 本表更新。

## 验证
`tests/test_cc500.sh`：宿主 hostcc 层（F-1/F-2/F-3 症状对立 + 各里程碑编译断言）+
guest 层（ccboot 自举 P1==P2 + ccrun 运行语义，含 for/do/一元/^~/乘除模/优先级用例）。

## 已知限制
- 输入上限 64KB（编译器源码随特性长大，从 32KB 提升；`open_input` 显式报错不截断）；
- 除零 / `INT_MIN / -1` / 有符号溢出为 UB（x86 idiv 自然行为，与宿主 C 一致）；
- 单线程、无重入（编译期全局状态），递归下降有 `cc_depth` 512 护栏。

## 与内核的集成
- 源码 `cc500.c` 以原始字节 blob 嵌入内核（Makefile `cc500_csrc.o`），boot 时写入
  initramfs `/cc500.c`；
- 编译产物可经 shell `ccrun <src> <out>` 调用，`ccboot` 做二次自举字节一致性校验。

## 许可证边界
本组件为独立 GPL 组件，与仓库其余部分（MIT）分开授权：**使用 / 修改 / 再分发** 本组件请按
GPL-2.0-or-later 履行义务（随衍生作品提供一份 GPL 许可副本等），勿将本组件当作仓库根
`LICENSE`（MIT）覆盖的代码。