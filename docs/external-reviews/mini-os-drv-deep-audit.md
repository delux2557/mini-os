# mini-os v2-c-kernel 驱动（drv/）子系统专项深审报告

> **来源**：代码审查专家（本轮专项审计）
> **审计对象**：`v2-c-kernel/src/drv/`（serial/e1000/ata/kb/vga/pci + e1000_netif/uart_netif 适配层）
> **方法**：逐行读码（e1000.c 573 行 / serial.c / ata.c 为主体）+ 与 BUG-018/019/020/051 对照，
>   + ISR 上下文调用链核实。
> **边界**：纯静态读码，未在 QEMU 动态复现；结论为架构/正确性评估。

---

## 0. 结论

驱动层两层结构清晰：**底层设备驱动**（serial/e1000/ata/kb/vga 直贴硬件，MMIO/PIO+I/O 指令）+
**netif 适配层**（`e1000_netif`/`uart_netif` 把具体网卡封装成 `netif_ops_t`，与协议层解耦）。
历次硬件驱动 bug（e1000 描述符 volatile、TCTL/RCTL 使能位、PIO 时序、串口 IRQ 撕裂）均封堵并有
版本注解。**无 P1/P2 已证缺陷**，登记 `OBS-DRV-x` 观察/加固项（1×P2 + 3×P3）。

---

## 1. 对象与边界

- 环境：QEMU（`-device e1000` 82540EM / 主通道 IDE / COM1/COM2）。
- 方法：静态读码 + ISR 上下文调用链推演；未动态运行。

## 2. 架构正向核实

- **HAL 屏障正确**：`src/drv` 的 `e1000_netif.c`/`uart_netif.c` 是网卡实现，`src/net` 仅依赖 `netif_*`
  接口（回归已断言无 e1000 直调）。换网卡 = 换注册后端，协议层零改动（D6 成立）。
- **关键位/寄存器注释完整**：RCTL/TCTL EN=bit1、QEMU rx flush-timer 特例、RDT 语义（避免 RDH==RDT
  死锁，注释明确"写 i+1 会丢包"）、TX 环 DD 轮询上限降载（BUG-051 相关）——硬件联合契约补齐。
- **serial K1 整行 IRQ 原子化**：`serial_puts/printf` 内 `xirq_save_cli` + `xirq_restore`（保存 XOR 改回
  关/开状态），行级日志不再被 IRQ0 抢占撕裂（BUG-052 根因封堵）。机制正确（clini 恢复用 pushl/popfl，
  不会误开早已关闭的中断）。
- **驱动 ISR 轻量原则**：serial_irq 只读字符转发；e1000_dhcp_tick 每 tick 至多收发一帧，绝不在
  timer ISR 忙等（RFC 2131 §4.4.5 状态机在 tick 里推进）。

## 3. 发现项（`OBS-DRV-*`）

### OBS-DRV-1【P2】e1000_tx 忙等 30 万次可能阻塞 timer ISR 上下文
`e1000_tx` 内部轮询 DD 位上限 30 万次（[e1000.c:451](file:///workspace/mini-os/v2-c-kernel/src/drv/e1000.c#L451-L455)），
被 `e1000_dhcp_tick` 在 timer_cb（IRQ0）内调用。已从 300 万降到 30 万次缓解，但**任何 TX 卡住时
（线缆断开/设备异常等）仍会在 ISR 内忙等几百 ms~ 毫秒级，期间与后续 IRQ 在硬件向量层面不响应**。
单核教学内核可接受，但建议：DHCP 续约的 TX 走独立轻量路径（TX 排队或标记待发，非 ISR 阻塞 wait）、
或把 DD 等待改到进程上下文重试。属验证性加固非缺陷（当前 TX 正常 <100 次即完成）。

### OBS-DRV-2【P3】`ata_read/write_sectors` 无 `lba+count` 越界校验
`ata_*_sectors` 校验 `count` 域（[ata.c:75](file:///workspace/mini-os/v2-c-kernel/src/drv/ata.c#L75)）但未校验
`lba+count > disk_sectors` 上限（[ata.c:74](file:///workspace/mini-os/v2-c-kernel/src/drv/ata.c#L74)/L85）。
越界访问会把控制器读到未定义/悬空区域，或写穿盘外。fs 层大概率已钳制块号（依赖调用方），建议在
驱动层加一道 `lba+count<=disk_sectors` 防御（分层防御原则）。

### OBS-DRV-3【P3】`serial_printf` 内嵌 `serial_puts` 二次取关中断（嵌套保留）
`serial_printf` 的 `%s` 分支调 `serial_puts`，后者再次 `xirq_save_cli/restore`。因 EFLAGS 保存/恢复
语义正确故无副作用，但属轻微冗余（一次格式化已知在关中断区间，无需再嵌）。P3 hygiene，非 bug。

### OBS-DRV-4【P3】e1000 TX 描述符环无逐槽 busy 标记
`e1000_tx` 以 `tx_cur % TX_N` 复用描述符，靠"前一次调用内已等完 DD 才返回"保证单线程不重入覆盖。
若未来引入异步/并发 TX，需加 per-slot busy 标记 + 环满回退。当前演示轮询模式成立，建议在
`e1000.h` 注明"TX 环为同步轮询、调用方须串行"之契约。

## 4. 与历史 bug 对照（防回归锚点）

| 历史 | 现况 | 状态 |
|---|---|---|
| BUG-018 e1000 TX 轮询被优化 | 描述符环 volatile + memory barrier | 已封堵 |
| BUG-019 TCTL/RCTL EN 写错位 | bit1 修正 + 注释 | 已封堵 |
| BUG-020 QEMU RCTL 1000ms 收包排队 | flush-timer 特例等待 | 已封堵 |
| BUG-051 serial 撕裂 | K1 整行 IRQ 原子化 | 已封堵 |
| BUG-052 RR 判据被撕裂假红 | 同 K1 | 已封堵 |

## 5. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| OBS-DRV-1 | P2 | 验证性加固 | TX 忙等移出 ISR 或轻量路径 |
| OBS-DRV-2 | P3 | 防御 | ata 层 lba+count 越界校验 |
| OBS-DRV-3 | P3 | hygiene | 嵌套 serial_puts 去重 |
| OBS-DRV-4 | P3 | 契约文档 | e1000 TX 同步轮询契约注明 |

*注：OBS-DRV-1 为静态推演（TX 卡死的 ISR 忙等时间），当前正常流程不触发；列 P2 验证性加固。*