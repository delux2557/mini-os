# mini-os v2-c-kernel 内存管理（mm）子系统专项深审报告

> **来源**：代码审查专家（本轮，独立于架构总览的专项审计）
> **审计对象**：`v2-c-kernel` 工作树 `d3b0c68`（BUG-053 收口后）
> **审计范围**：`src/mm/`（mem.c/pageing、heap.c、brk.c、mem.h）+ 跨子系统记账路径
> `sched.c`（fork/exec/exit/terminate/reap 帧归属）+ `usermode.c`（sys_map_page/sys_brk/sys_shmem/
> ELF load）+ `guard.c`（栈守卫判定）+ `kernel.c`（自审计调用）
> **方法**：逐行读码 + 全源码 grep 核实分配器调用上下文 + 记账/生命周期/并发逐路径推演。

---

## 0. 结论

内存管理子系统**记账路径达到"可审计正确"**：帧位图、堆双计数、freelist walk 三重对账防线
(`mem_audit` / `heap_audit` / `sched_audit`)，历史 5 个内存类 bug（
[BUG-027](file:///workspace/mini-os/docs/bugs.md#L577)/[BUG-028](file:///workspace/mini-os/docs/bugs.md#L597)/[BUG-033](file:///workspace/mini-os/docs/bugs.md#L725)/[BUG-034](file:///workspace/mini-os/docs/bugs.md#L744)/[BUG-035](file:///workspace/mini-os/docs/bugs.md#L759)）全部封堵并带版本注解。无 P1 级问题。

共登记 5 项 `OBS-MM-x` 观察/加固项（3×P3，2×P2），无阻塞。

---

## 1. 对象与边界

- 环境与方法：静态读码（`mem.c`/`heap.c`/`guard.c`/`brk.c`/`usermode.c`/`sched.c` 相关段）+ 全 `src/`
  grep `frame_alloc|frame_alloc_run|kmalloc|kfree` 的**所有调用点**逐一鉴定上下文（boot/syscall/
  IRQ/进程内）。
- 边界承诺：本轮仅读码与静态推演，**未改动代码、未运行**；结论为架构/健壮性评估而非动态缺陷证明。

## 2. 防御面核实（正向，均成立）

### D1 · 无锁模型的必要前提已满足：分配器绝不在 ISR 上下文调用
全源 grep 确认 `frame_alloc/frame_alloc_run/kmalloc/kfree` 的全部调用点分类：
| 上下文 | 调用点 |
|---|---|
| 引导/初始化 | `paging_init`、`mem_init`、`storage.c:173`(RAMDISK)、`kernel.c:29-46`(自检) |
| 系统调用 | `usermode.c` ELF load / `sys_map_page` / `sys_brk` / `sys_shmem` |
| 进程创建/切换 | `sched_fork` / `sched_exec` / `sched_new` / `stack_init` · 继承 |
| 回收 | `release_priv_frames` / `terminate_current` / `reap_process` |
| **中断ISR（kb/serial/e1000/定时器）** | **空（grep 无任何分配器调用）** |

→ 帧位图与堆链表的单核无锁访问**没有竞态触发点**，`BUG-051` 已把串口打印也做成整行 IRQ 原子。
这是无锁设计站得住的决定性证据，应写进设计文档作为**不变量**而不是隐性事实。

### D2 · "用户态缺页=隔离杀进程=OS 继续"为真（非整机停机）
`pf_handler` 用户态异常路径 `sched_kill → terminate_current`：先 `release_priv_frames` + 清 fd/socket，
唤醒等待者，再 `schedule(r)` **切到下一进程**；其后的 `cli;hlt` 是死代码（`schedule` 经
`sched_switch_esp` 换栈且不回返，[sched.c:748-750](file:///workspace/mini-os/v2-c-kernel/src/kernel/sched.c#L744-L750)）。
⇒ 任意用户 NULL/野指针/越界**只杀掉该进程**，其余进程与内核继续运行。隔离语义真实成立。

### D3 · 记账三重防线 + 历史 bug 封堵闭环
- `mem_audit`：`used_frames == 位图位数` 配平；`heap_audit`：freelist walk `free_sum/used_sum` 对账
  计数 + magic/free 一致性 + **成环上界**（`max_blocks = page_count*PAGE/24+4` 防 next 成环死循环）。
- 动态记账消除固定上限：`own_frames`/`fork_frames` 按需 `kmalloc`（[BUG-035](file:///workspace/mini-os/docs/bugs.md#L759)/OBS-002）；
  `map_frames[8]` 超限在分配**前**拒绝（[usermode.c:655-658](file:///workspace/mini-os/v2-c-kernel/src/kernel/usermode.c#L655-L658)），
  杜绝"映射后不记账"。

### D4 · 页表写后 TLB 安全 + OOM 降级不写物理0
`map_page_in` 写页表后 `invlpg`（[mem.c:166](file:///workspace/mini-os/v2-c-kernel/src/mm/mem.c#L163-L167)）；页表帧 OOM 返回 -1 且不写 `(uint32_t*)0`
（[BUG-033](file:///workspace/mini-os/docs/bugs.md#L725)），调用方降级：`pf_handler` 栈生长失败转 STACK_BOOM、
`fork_oom` 干净回滚（free fork_frames + addr_space_destroy + kstack）。

### D5 · 地址空间生命周期划分清晰，无双重释放
`addr_space_destroy` 只回收进程独占用户半区**页表帧+页目录帧**；数据帧走 PCB 记账各自独立；
fork 深拷贝跳过 SHMEM（保持跨进程共享，非深拷，[Violation 无]）。exec 先 `release_priv_frames + destroy`
再装新程序，明确无重叠归属。

---

## 3. 发现与观察项（`OBS-MM-*`）

### OBS-MM-1【P3】内核集中式堆只增不还（架构观察）
`kmalloc` 经 `heap_add_pages` 申请帧后**并入堆链即再不归还帧池**；`kfree` 只做相邻合并（O(n²) 重扫，
注释已注明 <20 块）；
`sys_brk` 收缩同样保留映射不做分页归还。后果：
- 长期运行 `free_memory_kb` 单调"假性"下降（堆吸水不是真泄漏，但难以区分）；
- 大 `kmalloc` 依赖 `frame_alloc_run` **连续帧**，与 fork/exec 深拷贝竞争同一帧池，碎片化下概率性失败。
建议：`heap.h` 注明"堆不回援帧池"；roadmap 加"整块空闲页归还"（当某空闲块覆盖整页且前后页整页空闲时 `frame_free` 回帧池）。

### OBS-MM-2【P3】`frame_alloc` 为 O(nframes) 首次适配
每次分配线性扫位图；fork 深拷贝为 `d × nframes`。当前规模（nframes≤16128）无碍，但注释未声明
复杂度假设，勿在并发/循环热路径误当 O(1)。可给 allocator 加"last-color / next-fit 游标"降摊还。

### OBS-MM-3【P3】`mem_init` 位图清零到 `nframes/8` 字节边界
`nframes` 非 8 倍数时末字节高位置脏（.bss 初为 0 故无实害）。建议改 `memset` 到覆盖 `nframes` 的整字节。

### OBS-MM-4【P2】ZOMBIE 持有页目录/页表帧直到 reap
`terminate_current` 立即释放数据帧，但 `page_dir`/页表帧延迟到 `reap_process`（父 wait 或心跳）。
符合"就地复用内核栈"的设计；若未来进程密度升高，可考虑 kill 即 `addr_space_destroy`（需保证僵尸不再
被调度）。观察项，当前不修。

### OBS-MM-5【P2】每进程资源配额散落定义（=resource_t 落地点）
用户栈槽 `USER_STACK_*`、堆 `USER_HEAP_*`(80页)、`map_frames[8]`、`own/fork_frames` 动态、
`fork_fcount` 等配额与回收循环散落 `mem.h`/`sched.h`/多处回收点，**任何一处扩容需同步改多处**。
建议收敛为一份"每进程资源配额"结构（呼应架构总览 `resource_t` 建议），并让 `mem_audit` 打印
每个活进程的资源占用汇总，使"超配/泄漏"在运行时即可显形。

---

## 4. 与历史 bug 的对照（防回归锚点）

| 历史 | 现况 | 状态 |
|---|---|---|
| BUG-027/028 记账数组泄漏 | 动态数组 + 成功/失败路径均 `kfree` | 已封堵 |
| BUG-033 页表帧 OOM 写物理0 | `map_page_in` 返回 -1 + `invlpg` + 调用方降级 | 已封堵 |
| BUG-034 行缓冲合并 | kb 行缓冲归属隔离（非 mm，相关风险域） | 已封堵 |
| BUG-035 fork_frames[24] 硬编码 | 先数后 `kmalloc` | 已封堵 |
| OBS-002 fork 动态数组回收 | `fork_oom`/`release_priv_frames` 双处归还 | 已封堵 |

---

## 5. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| D1 结论固化为文档不变量 | P2 | 文档 | 把"分配器勿在 ISR 调用 + 单核原子段"写进 design.md |
| OBS-MM-1 | P3 | 加固 | 堆空闲整页回帧池（可选，教学定位可推迟） |
| OBS-MM-5 | P2 | 重构 | 收敛为 resource_t 配额表 + 运行时资源汇总审计 |

*注：本审计为静态读码推演，未运行动态验证。若需，可由 `BUG-027~035` 复现探针或
`mem_audit/heap_audit` 压力场景作动态交叉验证。*