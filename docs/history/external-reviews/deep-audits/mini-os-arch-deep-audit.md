# mini-os v2-c-kernel arch（arch/）层专项深审报告

> **来源**：代码审查专家（本轮专项审计）
> **审计对象**：`v2-c-kernel/src/arch/`（boot.s 65 行 / idt.c 121 行 / isr.s 129 行 / timer.c 30 行）
> **方法**：逐行读码 + 中断门/上下文切换/时钟时序推演 + 与 BUG-001/002/009/051 对照。
> **边界**：纯静态读码，未动态运行；结论为架构/正确性评估。

---

## 0. 结论

arch 层**精简且联合契约闭环**：multiboot 引导 → 平坦 GDT → 统一中断现场；上下文切换复用
中断帧（`resume_point`），无重复寄存器保存；PIC 只开放 3 个 IRQ；timer 心跳驱动 DHCP 续约与
调度。**无 P1/P2 已证缺陷**，登记 `OBS-ARCH-x` 观察/加固项（4×P3）。

---

## 1. 对象与边界

- 环境：QEMU + legacy 8259 PIC + PIT 8254。
- 方法：静态读码 + 中断门自动关中断语义推演；未动态运行。

## 2. 架构正向核实

- **IDT 中断门自动关中断 → 桩无需显式 cli**：`set_idt_gate` 用 flags `0x8E`（present+32 位中断门，
  [idt.c:63](file:///workspace/mini-os/v2-c-kernel/src/arch/idt.c#L63)）；CPU 经中断门进入时**自动清 IF**，
  故 `isr_common_stub` 压现场/调 C 期间中断天然关闭，无需 `cli`。多上下文读共享结构安全。
- **DPL3 系统调用门**：`set_idt_gate(128,...,0xEE)`（[idt.c:116](file:///workspace/mini-os/v2-c-kernel/src/arch/idt.c#L116)）
  使用户态可 `int 0x80`；该门同样自动关中断，syscall 处理原子。
- **上下文切换复用中断帧**：[isr.s:98-115](file:///workspace/mini-os/v2-c-kernel/src/arch/isr.s#L98-L115) 的
  `resume_point` 从 gs 槽 `pop gs..popa; add esp,8; iret` 恢复全场；`sched_switch_esp` 仅 `mov esp,eax; jmp`
  直接切栈（[isr.s:123-127](file:///workspace/mini-os/v2-c-kernel/src/arch/isr.s#L123-L127)）——与正常中断返回
  共用同一路径，无需额外保存。BUG-001（依赖 `[esp-4]` 返回地址）已由此根治。
- **EOI 先于处理函数**：`isr_handler` 对 IRQ 先发主/从片 EOI 再调处理函数（[idt.c:85-89](file:///workspace/mini-os/v2-c-kernel/src/arch/idt.c#L85-L89)）——
  定时器处理函数会切换进程、不再返回，必须先 EOI。语义正确（BUG-002 的 `cli;hlt` 死循环域已避开）。
- **timer 心跳优先级**：`timer_cb` 先 `e1000_dhcp_tick`（非阻塞状态机），再 `sched_tick`（可能抢占/切换），
  DHCP 续约不被切换跳过（[timer.c:15-21](file:///workspace/mini-os/v2-c-kernel/src/arch/timer.c#L13-L22)）。

## 3. 发现项（`OBS-ARCH-*`）

### OBS-ARCH-1【P3】PIC 从片被全掩码，从片 IRQ8-15 永不触发
`pic_remap` 掩码主片 `0xEC`（仅开 IRQ0/1/4）、从片 `0xFF`（全掩，[idt.c:73-74](file:///workspace/mini-os/v2-c-kernel/src/arch/idt.c#L73-L74)）。
当前 3 个 IRQ 均在主片，故正确；但若未来挂主片级联从片的设备（RTC、PS2 辅助、SATA IRQ14/15 等），
会因掩码而静默失效。建议在此注释显式声明"从片设备当前未启用，绑定主片 IRQ"，避免未来误用。

### OBS-ARCH-2【P3】`sched_switch_esp` 入参类型为裸 `uint32_t`（esp 栈地址），无类型安全
`extern void sched_switch_esp(uint32_t esp)`（[sched.h:95](file:///workspace/mini-os/v2-c-kernel/src/kernel/sched.h#L95)）
接收 gs 槽地址。若调用方误传非栈地址会直接跳入并 `iret` 崩溃。P3 观察：可改 `const registers_t*` 语义或
加注释契约，当前调用点（terminate/sched_start/schedule）均传 `kernel_esp` 正确。

### OBS-ARCH-3【P3】boot.s 引导无热重启错误上报（hang 前不打印）
`_start` 的 `.hang`（[boot.s:38-41](file:///workspace/mini-os/v2-c-kernel/src/arch/boot.s#L38-L41)）在 `kernel_main`
返回后 `cli;hlt`——若 `kernel_main` 异常返回（正常应不返回），仅静默停机无诊断。P3：可在 hang 前输出
一行标记，便于引导期失败排查。

### OBS-ARCH-4【P3】timer 心跳完全依赖单次 `divisor = 1193182/freq`，无 `freq=0` 保护
`timer_init` 直接做 `1193182/freq`（[timer.c:25](file:///workspace/mini-os/v2-c-kernel/src/arch/timer.c#L24-L25)），
若调用方传 `freq==0` 会有除零（当前仅 kernel.c 调一次固定值，无实害）；P3：加 `freq==0` 早退或断言。

## 4. 与历史 bug 对照

| 历史 | 现况 | 状态 |
|---|---|---|
| BUG-001 上下文切换依赖 [esp-4] 返回地址崩溃 | sched_switch_esp jmp resume_point | 已封堵 |
| BUG-002 idle `cli;hlt` 关闭中断死锁 | 正常返回路径 | 已封堵 |
| BUG-009 引导期过早开中断让调度抢跑 | 时序编排 | 已封堵 |

## 5. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| OBS-ARCH-1 | P3 | 文档 | 从片掩码=未启用的显式注释 |
| OBS-ARCH-2 | P3 | 契约 | sched_switch_esp 类型/注释契约 |
| OBS-ARCH-3 | P3 | 加固 | boot hang 前输出诊断标记 |
| OBS-ARCH-4 | P3 | 防御 | timer_init freq=0 早退 |

*注：全项为静态推演的观察/加固，无已证崩溃；当前 3-IRQ、固定 freq 场景下均正确。*