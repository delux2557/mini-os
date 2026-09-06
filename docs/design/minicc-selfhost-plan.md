# minicc 自举路线图（规划存档）

> 状态：**规划 / 未执行**。约束：`minicc` 有 dev 在改，**全部只规划、暂不动 minicc 代码**。
> 终态（北星）：**minicc 能编译自己的权威源码 `minicc.c`（真自举）**，顺带消化 cc500 方言、
> 把三方互锁外推到 cc500 全集。

## 背景与现状（实证核对，2026-09-06）

- **两套实现**：`minicc.c`（68,097B，rich C，宿主 gcc 编 = hostminicc）与 `minicc_self.c`
  （44,913B，minicc 子集誊写版，minicc 可编）。
- **自举现状**：minicc 有 `miccboot` 自举测试（P1 编译自身 → P2，P1==P2 byte-identical），
  但自举对象是 **`minicc_self.c`**，不是权威源码 `minicc.c`。
- `hostminicc` 编 `minicc.c` → **`input too big (>64KB)`**（68KB 超上限）；编 `minicc_self.c` → OK。
- `hostminicc` 编 `cc500.c` → 卡在**顶层 extern 原型**（`int syscall3(...);` / `int getchar(void);`）。

## 两条支路（共用前半段）

- **支路①**：minicc 吞并 cc500 方言（编得动 `cc500.c`）。
- **支路②**：minicc 编自己 `minicc.c`（真自举）。
- 两者都要先过「体积关 + 语法关」，之后语法关各自补齐各自缺口。

---

## Phase Zero · 抬输入上限（前置，二者共同瓶颈）

- `minicc.c`：`xmalloc(65536)` 与守卫 `if (in_len >= 65536) … (">64KB")` → 抬到 **128KB**。
- `minicc_self.c`：`xmalloc(65536)` 与 `if (in_len >= 60000) fail("input too big")` → 同步抬到 128KB。
- 成本：输入缓冲多开几十 KB 堆；guest `-m 64` 无压力。
- 验收：`make test-minicc` + `miccboot` 全绿（纯容量改动，不动语义）。

## Phase A · 语法容忍（声明层两小改）

- A1：顶层 `type name(params) ;` 原型 → 登记为「仅声明」符号，调用走既有 undefined 处理。
- A2：原型形参允许缺名（`(int,int)` 而非 `(int x, int y)`）。
- 验收：`test-minicc` 全绿 + `minicc_self.c` 自举不变。

## Phase B · 支路①：吞并 cc500 方言（新 CI 锚点）

- B1：新增断言 `hostminicc 编 cc500.c → compiled OK`（管住「吞下方言」不退步）。
- B2：`shim_crt.c` 把 cc500 微 libc（`malloc/exit/getchar/syscall3/sys_print`）映射到 minicc
  产物可运行的 host 函数 → 跑产出的 cc500 → 让它再编自身 → 与 `cc500(P1==P2)` 差分对拍。
- B3：把 `cc500.c` 收入 diffsynth acceptance 语料（minicc 拒即红）。

## Phase C · 支路②：minicc.c 真自举（语法关，单独工作量）

- 抬上限后实测 `hostminicc 编 minicc.c`，逐一清 minicc.c 用到的**子集外特性**（结构体、`#define`/
  `#include`、不支持的写法等），列成缺口清单，逐个决定「minicc 补语法」还是「minicc.c 降级到子集内」。
- 验收：`miccboot` 改用 `minicc.c`（而非 `minicc_self.c`）作「自身」，P1==P2 byte-identical。
- 愿景：真自举达成后，minicc 只保留一份权威源码，`minicc_self` 退居纯 bootstrap 形态。

## Phase D · 三方互锁外推（可选增强）

- 支路①②都通后，diffsynth 三方（gcc/minicc/cc500）对拍的公共子集可外推到 cc500 全方言乃至
  minicc 全方言，织进既有三方互锁。

---

## 顺序与工作量

- **Zero → A → B / C（可并行）→ D**。
- Zero + A 约两个小补丁量级；受 dev 约束暂缓执行。

## 关键验收锚点清单

- [ ] Phase Zero：`make test-minicc` + `miccboot` 绿（抬 128KB）。
- [ ] Phase A：`hostminicc 编 cc500.c` 过 extern 原型。
- [ ] Phase B：`cc500.c` 纳入 diffsynth acceptance；产出的 cc500 与 `cc500(P1==P2)` 差分一致。
- [ ] Phase C：`miccboot` 以 `minicc.c` 为自身，P1==P2 byte-identical。