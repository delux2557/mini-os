# 版本变更日志（Changelog）

> 格式遵循 Keep a Changelog 精神：每个版本列出 Added / Changed / Fixed / Engineering。

## \[v0.32] - 2026-08-31 · 修复 cc500 编译器三缺陷（F-3 字符串自噬 / F-2 未定义静默 / F-1 关系运算残缺）

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

## \[v0.31] - 2026-08-31 · 内核资源归属收口：per-process fd 表 + socket 归属/回收

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

## \[v0.30] - 2026-08-30 · 修复工具链严重 BUG（文件槽泄漏 + 自编译产物丢 argv）

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
* **BUG-033：`map_page_in` 页表帧 OOM 写物理 0 破坏内核**（`src/mm/mem.c/h`，
  代码审查 P0）：新页表 `frame_alloc()` 失败返回 0 未检查 → `(uint32_t*)0` 清零低 4KB
  且页目录项指向物理 0；`pf_handler` 栈生长静默失败 → 假增长死循环。修复：`map_page_in`
  改返回 `int`（-1 页表帧 OOM），`pf_handler` 检查失败转 `STACK_BOOM` 并释放已分配帧
* **BUG-034：kb 行缓冲在 `line_ready` 期间仍追加输入**（`src/drv/kb.c`，代码审查 P2）：
  行就绪未取时新可打印字符追加到旧行后，`kb_line_take` 取行时丢失。修复：仅
  `!line_ready` 才入缓冲；`test_kb.c` 补用例 13
* **BUG-035：`fork_frames[24]` 硬编码限制大进程 fork**（`src/kernel/sched.c/h`，=OBS-002，
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
  （qemu_regression.sh / test_serial.sh）同步 v0.30；Makefile cc500 单文件豁免
  精简为 `-Wno-int-conversion -w`（GCC 8+ 两者并存均有效——GCC 14 中
  `-Wint-conversion` 是 permerror 硬错误，`-w` 压不住，必须显式 `-Wno-*`；GCC 13
  下 `-w` 单独够用是曾误删此项的根源，见 BUG-036）
* **BUG-036（v0.30 内部回归）**：L-5 误删 `-Wno-int-conversion` 只留 `-w` →
  GCC 14 环境 `make` EXIT=2 构建失败（-Wint-conversion ×13）；恢复该项后
  修复。记录于 docs/bugs.md，教训：`-w` 不压 permerror，GCC>=14 需显式 `-Wno-*`

## \[v0.29] - 2026-08-30 · 加固：宿主侧 fuzz + 内核堆审计

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

* **BUG-029：`icmp_parse` 短帧越界读**（fuzz 抓到）：`len < 14` 时 `frame + 14` 越过帧尾、
  `len - 14` 无符号下溢成巨大值，`ip_parse` 按巨大长度扫载荷 → 堆缓冲区越界读。修复：
  `icmp_parse` 开头 `if (len < 14) return -1`；`test_icmp.c` 补 13/0 字节短帧回归断言
  （BUG-029 已在 `docs/bugs.md` 记录）
* **BUG-030：fork 子进程在继承的已生长栈上继续递归被误判缺页**（回归盲区补格抓到）：
  `sched_fork` 把子进程 `user_esp_top/stack_bottom` 原样继承 → 子进程栈在**父进程栈槽**；
  `stack_guard_hit` 却按**子 pid** 反推槽位，子进程下探时 fault 判"槽外"（STACK_OK）→
  走普通缺页被隔离终止。修复：槽位改由**实际栈位置 `stack_bottom`** 推导
  （`stack_bottom & ~(USER_STACK_SLOT-1)`），普通进程=自身槽、fork 子进程=继承的父槽，
  两者皆正确；v0.15 边界语义由真实栈槽天然保持（`src/kernel/guard.c`）

**Engineering**

* **回归盲区补格**：新增 `deepfork`（已生长栈×fork）与 `deepexec`（已生长栈×exec）
  演示应用，挂入 qemu_regression.sh / test_serial.sh（交互 + 全量校验双层断言）；
  第三个盲区项「brk 收缩-再涨」由 heapdemo 既有 step 4 覆盖、「编译产物×持久化」
  由 test_persist.sh 的 S10（writefile→ccrun→save→重启→run）覆盖
* `test_guard.c`：补 4 条 fork 继承栈守卫断言（旧 pid 推导逻辑下必失败）
* `test_heap.c`：宿主为 64 位、`block_t` 实际占 24 字节（`next` 8B），审计篡改用例用同构
  probe 结构 + `offsetof` 定位 magic 字段，跨 32/64 位宿主通用；并在高强度
  分配/释放（分裂/不分裂/合并/复用全路径）后断言 `heap_audit()==0`
* `qemu_regression.sh` selftest 断言新增 `[audit] heap ok`
* 验证：宿主测试 16/16 全绿；内核 -m32 编译零告警；QEMU 回归全量通过
  （含 deepfork/deepexec 组合 + selftest `[audit] heap ok`：
  `4 blocks, free 3 blocks/102320B used 16B pages=25`，used+free+4×16B 头 = 25×4096
  精确守恒）；串口回归 + ATA 持久化回归通过；200 万轮 fuzz 复核无崩溃

## \[v0.28] - 2026-08-30 · DHCP 租期续约（T1/T2 renew，RFC 2131 §4.4.5）

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
  用户 socket 的 recvfrom 会"排空"网卡（netsock_drain 取走 NIC 环所有帧），无匹配
  本地端口的 DHCP 应答会被抢先丢弃（sockdemo 每 tick 轮询即踩中）；注册端口 68 的
  DHCP socket 后，分发路径把应答入其队列，续约 tick 经它读取（与用户流量共享分发）

* **续约帧构建**（`src/net/dhcp.c/h`）：`build_bootp` 支持 ciaddr 与
  with_server_id（REBIND 不含 54）；新增 `dhcp_build_renew`（单播）/ `dhcp_build_rebind`（广播）

**Fixed**

* **BUG-027：`sys_map_page` 记账槽满时帧泄漏**：`map_frames[8]` 满后第 9+ 张帧被映射
  但不记账、进程退出不回收。改为**分配前拒绝**（`map_fcount >= 8` 返回 -1），诚实暴露
  教学上限而非悄悄泄漏
* **BUG-028：exec 路径泄漏 `load_frames` 记账数组**：v0.26#3 把 load_frames 改为 kmalloc
  动态数组时，spawn 路径补了 `load_frames_free()` 但 exec 路径漏了——每次 exec（成功或失败）
  泄漏该数组。修复：`sched_exec` 复制进 `own_frames` 后 `kfree(frames)`（参数改非 const，
  语义=移交），exec 失败路径补 `load_frames_free()`
* **e1000_dhcp_tick 用户页目录下访问高地址 MMIO 缺页**：e1000 MMIO 位于
  0xFEB00000（PDE≥512），用户进程页目录只克隆低 1GB PDE；timer ISR 可能在任意用户进程
  上下文运行 → 访问设备寄存器即 `[FATAL] page fault @feb83818`。修复：tick 内临时切内核
  页目录（与 netsock 收发同款），用完切回（须在 sched_tick 前恢复）
* **print_ip 缺 `& 0xFF`**：`(ip>>24)` 等未掩码导致 IP 显示成 `10.2560.655362.167772687`
  （预存在 v0.25 的显示 bug，功能不受影响；续约日志与 OFFER/ACK 均走该函数，一并修正）

**Engineering**

* 宿主单测 `tests/test_dhcp.c` 38→61 断言：RENEW/REBIND 帧结构——ciaddr 写入 BOOTP 头、
  RENEW 单播（dst_mac/目标 IP）带 54+50、REBIND 广播仅带 50（无 54）、UDP round-trip
* `tests/test_net.sh`：用短租期内核（`make DHCP_RENEW_SECS=2`，Makefile `-D` 注入
  e1000.o）在秒级窗口内观察续约闭环——新增断言 `renew: sent RENEW (unicast)` 与
  `renew ACK`；DURATION 循环会因 sockdemo 提前 break，须等续约出现再杀 QEMU（否则
  早杀漏掉 tick=100 的首次 RENEW）；pcap UDP 计数 10→12（RENEW+ACK 双向）
* Makefile：新增 `DHCP_RENEW_SECS` 变量（缺省用服务器租期，SLIRP 为 24h）；
  test_net 跑完恢复常规内核
* 代码审查驱动：design.md §3 明确**单核假设**并发模型（sem/msg 的 try+block 在单核 +
  关中断模型下安全）；shell.c `file_copy/equal` 标注全局 fs 槽位 1/2 占用（per-process
  fd 表 TODO 见 roadmap 支线 C）；bugs.md 新增 BUG-027/028 与 OBS-002（fork_frames[24]
  硬编码上限观察项，动态化列入后续版本）
* **项目方向调整**：roadmap 纳入"收尾-加固-沉淀"三阶段路线 + 红线清单（README 定位同步）
  ——功能闭环已达成，后续不追逐版本号，转向 fuzz/record-replay 加固与教学文档沉淀
* 五层回归全绿（宿主 15/15 + qemu + serial + persist + net）

## \[v0.27c] - 2026-08-30 · 评估反馈修复（GCC 14 构建卫生 + brk 守卫口径 + 回归补格 + 文档回补）

**Fixed**

* **S8：`brk_pages_up` 容量守卫记账口径**（`src/kernel/usermode.c` `sys_brk`）：原守卫
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
  共享内存 0x801A0000 / 堆区 0x801A4000 320KB / USER_SPACE_END 0x81000000）；
  §10 测试层数四层→五层（+网络回归）+ selftest 计数 5→6；
  新增 §17 网络子系统（v0.18-0.25）、§18 容量三连（v0.26）、§19 工具链与自举（v0.27）
* **S3：README 版本矩阵**补 v0.10-v0.27b 行（此前停在 v0.9，连续第 3 次被点名）
* GCC 14.2 clean build 验证：`make` 零告警，`kernel.elf` 329KB；五层回归全绿
  （宿主 15/15 + qemu 176 项 + serial + persist（含 S10 新用例）+ net）

## \[v0.27b] - 2026-08-30 · 写-编-跑演示闭环：cc500 支持命令行路径

**Added**

* **cc500 命令行指定输入/输出路径**（`tools/cc500/cc500.c`）：`cc500_main(argv, argc)`
  按 `argv[1]=输入 argv[2]=输出` 取路径（`load_ptr` 逐字节拼回 4 字节指针，因 CC500
  无 int\* 解引用/类型转换），缺省回退 `/cc500.c` → `/out.elf`（`run cc500` 保持原行为）；
  入口/CRT 声明顺序对齐 CC500 反向压参与内核 `[esp+4]=argc,[esp+8]=argv` 的差异

* **shell `writefile <path> <content...>`**：把命令行剩余部分（保留空格）写入文件，
  让 agent 能在 guest 内经 shell 写源码（ARG_MAX 32→128 解除内容截断）

* **shell `ccrun <src> <out>`**：fork+exec cc500 编译 `<src>` 为 `<out>` → `run <out>`
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

## \[v0.27] - 2026-08-30 · 工具链与自举：guest 内「写-编-跑」闭环

**Added**

* **CC500 编译器移植**（`tools/cc500/cc500.c`，E. Grimley-Evans 自托管 C 子集编译器
  ~750 行）——v0.27-29「工具链与自举」的核心一步，**一步到位实现完整自举闭环**
  （原规划 27a 汇编器+链接器 / 27b C 前端 分两版，现以整机自托管编译器直接达成）

  * 链接基址改 `code_offset=0x800A0000`（APP_LINK），ELF 头 e_entry/p_vaddr/p_paddr
    同步改为 0x800A 基址，入口 stub 适配 mini-os ABI（`SYS_EXIT=0`）

  * **唯一机器码 stub = 通用系统调用** `syscall3(n,a,b,c)`（eax=n ebx=a ecx=b edx=c，
    int $0x80），`exit/malloc/getchar/putchar/sys_print` 全部改用 CC500 C 子集实现
    （malloc 基于 `SYS_BRK=35`），编译器内部不含任何平台相关代码

  * **输入输出走 mini-fs**：整读 `/cc500.c`（initramfs 预置源码）进堆做 `getchar` 源；
    `putchar` 为空操作，编译完由 `flush_output` 把 code 缓冲一次性写回 `/out.elf`
    ——绕开 mini-os 无 stdin/stdout 重定向的限制，与文件系统天然衔接

  * **专用 CRT**（`src/app/cc500_crt.c`）：`_start` 以 `cc500_main()` 返回值
    `sys_exit`（普通 crt.o 固定退出 0，无法把编译成败传给 shell）；不 include
    user_lib.h（其 static inline `syscall3` 会与外部 `syscall3` 重名冲突）

* **initramfs 嵌入**：`cc500`（编译器 ELF）+ `cc500.c`（自举源，objcopy 原始字节）
  两个文件；Makefile 新增 cc500 构建链（`tools/cc500/cc500.c` → 独立编译 →
  专用 crt 链接 → ELF blob + 源码 blob 嵌入内核）

* **shell `ccboot` 自举命令**：run cc500（gcc 版）编译自身 → `/out.elf`=P1；
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

## \[v0.26] - 2026-08-29 · 容量三连#1：用户栈按需生长

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

* **`deep` 演示程序**（`src/app/deep.c`）：递归分配 1KB 局部数组 ×12 层，在 4KB
  初始栈上触发 3 次按需生长后存活（`survived 12KB recursion via stack growth`）

**Engineering**

* 宿主单测 `tests/test_guard.c` 重写（34 断言）：覆盖三态边界——槽外/已映射页内
  → OK、当前守卫页（有空间）→ GROWTH、深越界/到硬底 → BOOM、生长后守卫页下移

* 回归升级：`tests/qemu_regression.sh` 与 `tests/test_serial.sh` 新增 `deep` 用例
  （启动日志 → `[stack] grow` → 存活 → 退出码 0）

* 版本横幅与 motd 更新为 v0.26；roadmap 勾选该项

**Fixed**

* `deep` 演示程序尾递归被 `-O2` 改写为循环、栈占用不足 4KB 不触发生长（见 BUG-024）

## \[v0.26#2] - 2026-08-29 · 容量三连#2：用户堆（brk/sbrk）

**Added**

* **用户堆系统调用**（`SYS_BRK=35`，`src/kernel/usermode.c` 分发）：`sys_brk(addr)`
  查询/设置 program break、`sys_sbrk(incr)` 相对增长（返回旧 brk）；堆区
  [USER_HEAP_BASE, USER_HEAP_MAX) 在共享内存之后（v0.26#3 定为 0x801A4000-0x801F4000，
  320KB = 80 页），每进程独立、相互隔离

* **堆状态纯逻辑**（`src/mm/brk.h` `brk.c`）：`brk_pages_up`（扩展需补映射页数）、
  `brk_in_range`（收缩/复用边界）独立成模块，可宿主单测；扩展按页补映射物理帧并
  记账进 PCB（`heap_frames[]` + `heap_fcount`），收缩只更新 `heap_brk` 保留映射复用

* **页错误堆处理**（`src/mm/mem.c` `pf_handler`）：访问已分配但未映射的堆页时按需补映射，
  与栈生长共用页错误路径；堆页访问越界（越过 heap_brk 且映射缺失）按普通缺页拒绝

* **`heapdemo` 演示程序**（`src/app/heapdemo.c`）：初始 brk 查询 → sbrk(4K) 写入校验 →
  sbrk(16K) 写入校验 → 收缩回 8KB 处（内核保留映射）→ sbrk 复用已映射页 → 极简
  bump-allocator 冒烟（编译器 malloc 铺路）

**Engineering**

* 宿主单测 `tests/test_brk.c`（21 断言）：brk 状态机边界——不动/收缩/页对齐扩展/超上限
* 回归升级：`tests/qemu_regression.sh` 与 `tests/test_serial.sh` 新增 `heapdemo` 用例
  （brk 查询 → 扩页日志 → 收缩保留映射 → 存活 → 退出码 0）

## \[v0.26#3] - 2026-08-29 · 容量三连#3：ELF 加载去上限

**Added**

* **ELF 加载去上限**：`usermode.c` 的 `load_frames` 由固定 8 项静态数组改为按需
  `kmalloc` 动态列表，`sched.c` 的 `own_frames` 同步动态化（`own_frames_take` 拷入
  PCB、`release_priv_frames` 归还），解除 32KB/8 帧约束，支持 MB 级 ELF；
  `APP_MAXFRAMES`/65536B 旧上限检查移除（上限改为 app 区同量级 1MB）

* **地址空间重布局**：用户空间扩至 16MB（USER_SPACE_END=0x81000000）；app 区扩为
  1MB（0x800A0000-0x801A0000），共享内存迁至 app 区之后（0x801A0000）、堆区迁至
  0x801A4000（v0.26#2 首版曾驻 0x800B0000，随本次迁址后断言同步更新）

* **`bigdemo` 演示程序**（`src/app/bigdemo.c`）：70KB 初值数据（.data 段）使 ELF 文件
  78KB > 旧上限 65536B，加载需 21 帧（旧 8 帧上限时代无法加载）；逐字节填充校验和
  验证数据完好

**Engineering**

* 宿主单测 `tests/test_userptr.c` 边界更新：USER_SPACE_END 扩为 0x81000000 后
  上限内末 4 字节 / 越界 / 长度溢出断言随新边界调整
* 回归升级：`tests/qemu_regression.sh` 与 `tests/test_serial.sh` 新增 `bigdemo` 用例
  （启动 → 70KB 校验 → 存活 → 退出码 0），并断言加载帧数突破旧 8 帧上限

## \[v0.25] - 2026-08-29 · DHCP 客户端：动态获取 IP/网关（静态可配置化）

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

## \[v0.24] - 2026-08-29 · UDP 校验和错误路径

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

## \[v0.23] - 2026-08-29 · ICMP Echo：PING 通宿主

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

## \[v0.21] - 2026-08-29 · 内核自审计 + syscall 边界契约化

**Added**

* 运行时自审计内建进内核：`sem_invariant_ok`（count+waiters 守恒）、`mem_audit`
  （used\_frames 与帧位图配平）、`sched_audit`（PCB 状态机合法性），由 syscall 34
  `SYS_KERN_AUDIT` 一键触发

* selftest 追加第 6 项，`[selftest] PASS (6 checks)` 从"5 个应用没崩"升级为
  "内核核心不变量成立"

* 调度日志（block/wake/exit）统一带单调 tick 戳；abuse 边界断言补齐至 17 项
  （exec argv / sendto·recvfrom iov / read·write buf 等内核地址一律 -1）

## \[v0.20] - 2026-08-29 · 网络可用：用户态 UDP socket

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

## \[v0.19] - 2026-08-29 · 网络加厚：极简 IP/UDP

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

## \[v0.18] - 2026-08-29 · e1000 网卡驱动 + 极简网络栈（PCI/ARP）

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

## \[v0.17] - 2026-08-29 · syscall 边界校验（copyin/copyout）

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

## \[v0.16] - 2026-08-29 · 用户态 CRT 收口 + ATA 真盘持久化 + 单行自检

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

## \[v0.15] - 2026-08-29 · 补全 wait()/waitpid 语义与孤儿清理

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

## \[v0.14] - 2026-08-29 · 文件系统增强：目录层级 / 间接块 / 偏移定位与追加写

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

## \[v0.13] - 2026-08-29 · 用户栈守卫页与栈溢出检测

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

## \[v0.12] - 2026-08-29 · fork/exec 进程模型与 argv 参数传递

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

## \[v0.11] - 2026-08-29 · 每进程地址空间与物理内存隔离

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

## \[v0.10] - 2026-08-29 · 串口终端：外部 agent 经 QEMU 交互

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

## \[v0.9] - 2026-08-29 · 可执行程序加载与交互式 Shell

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

## \[v0.8] - 2026-08-29 · 文件系统（内存盘 + 极简 mini-fs）

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

## \[v0.7] - 2026-08-29 · IPC：有界消息队列（生产者-消费者）

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

## \[v0.6] - 2026-08-29 · IPC 与同步（信号量 + 共享内存）

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

## \[v0.5] - 2026-08-28 · 抢占式多任务与进程调度

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

## \[v0.4] - 用户态 ring3 与系统调用

**Added**

* 重建 GDT（kernel/user 代码段 + 数据段 + TSS），`ltr` 加载 TSS

* `int 0x80` 系统调用门（DPL=3）：`exit / print / get_ticks / sleep / yield / get_pid`

* ring3 用户程序加载与运行（共享代码页 0x80000000）

* 内存保护演示：用户态写内核地址 0xB8000 → 页错误 → 进程终止

* 键盘回显主循环

## \[v0.3] - 内存管理

**Added**

* 物理页帧分配器（4KB 粒度）

* 分页开启 + 页表管理（`map_page`）

* 内核堆 `kmalloc/kfree`（首适应）

* 懒分配：缺页按需映射，`pf_handler` 支持懒分配区恢复

* 内存自检与状态行（free / heap / lazy）

## \[v0.2] - C 内核地基

**Added**

* multiboot 引导（QEMU `-kernel` 直接加载）

* GDT/IDT、8259 PIC 重映射、CPU 异常处理

* PIT 定时器（100Hz 心跳）、PS/2 键盘、VGA 文本 + 串口输出

* 交互式回显（可键入内容，回车换行）

## \[v0.1] - 引导与保护模式

**Added**

* 软盘引导扇区（`v1-floppy/`）

* 实模式 → 保护模式切换

* VGA 打印 "Hello Micro-OS!"

