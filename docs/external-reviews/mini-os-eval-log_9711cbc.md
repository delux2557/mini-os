# mini-os 独立测评运行记录

对象：main `9711cbc`
环境：Ubuntu 24.04 / gcc 14(-m32) / nasm / QEMU 8.2.2 (TCG) / python3 / socat
方法：T1=系统本身，T2=RR(rcd/rdpl) 基础设施可靠性。全程 `src/` 零改动；工作树仅供测评，产物在 build/ 与 build-logs/。

时间戳：2026-09-03 (Asia/Shanghai)

---

## 阶段1：基线盘点
| 目标 | 结果 | 备注 |
|---|---|---|
| 工具链补齐 | ✅ | qemu-system-x86 + gcc-multilib + libc6-dev-i386 |
| make 内核构建 | ✅ | kernel.elf 生成 |
| test-det (P1) | ✅ 绿 | 34 行 boot 确定性前缀逐字节一致 |
| test-tr (P2) | ⚠️ 首跑翻红→重跑绿 | 里程碑行 `[ls] /:` 前被注入多余字节 `l`；RR 复现性检查可抖动 |
| make test 12 层 | ⚠️ 11 层绿，仅 test-tr 红 | 唯一红点 = test-tr [4/4] 复现性（milestone 行 `l[ls] /:` 乱入） |
| test-repro | ✅ 绿 | BUG-A/BUG-B 未复现 + argv 硬断言过；tr_mark ready 锚点 log_off=14926 生效；transcript in=14/93340B |
| test-tcp-attack | ✅ 绿 | 3738 脏注入包下业务 8KB+TAIL 完整交付、RESULT PASS、内核无异常标记 |

> ⚠ **实证（R3/R4 判据可信度）**：`test-tr` 第[4/4]步"复现性雏形"共跑 3 次失败 2 次（~67%），
> 症状均为里程碑行 `[ls] /:` 前乱入一个回显字节 `l`（`ls` 命令首字符）。
> 属自述 F4 边界：无窗口归一化时 boot auto-demo/回显与命令流混排污染判据 → **判据本身会假红**。
> 这是 RR 基础设施"复现性时钟"脆弱性的量化证据。

---

## 阶段2：T2 RR 基础设施可靠性审计

### K1（T1·内核 确认）serial_printf 无 IRQ 原子性 → 日志行撕裂
- 证据：[serial.c L76-L97](v2-c-kernel/src/drv/serial.c) `serial_printf`/`serial_puts` 逐字符 `serial_putc`，整行不加锁/不关中断。
- 现象（rp_torture golden，torture-a out.tr L1048）：`[shell] 'nosuchprog[sched] sleep pid=1 10 ticks (wake@466)' exited code=1` —— shell 写一半被 timer 抢，sched 行插入，`[shell] 'nosuchprog' exited code=1` 被撕裂。
- 影响：① 串口取证不可靠；② **RR golden 自身非确定性** → 确定性/复原判据假红。
- 严重度：Low-medium（无内存/安全影响，但破坏可观测性并放大为 RR 阻塞项）。修复方向：`serial_printf` 全程 `cli/sti` 保证整行原子，或串口加行锁。

### R3/R4（RR 判据可信度）结论
| 测法 | 结果 |
|---|---|
| 同一 transcript 回放次数稳定（rp_torture D 段） | 复原契约集差异仅来自 golden 撕裂行 `' exited code=1`（golden 缺 `nosuchprog` 名），非 replay 逻辑错 |
| 两轮 icount 冷启契约行（rp_torture A 段） | 分歧 = 同一条撕裂行 + fork 子 pid 漂移(3/4 vs 5/9)；B 段标记扫描纯 procCrash 预期 |
| gate/判据可信度 | `func()` 依赖"行级干净输出"，撕裂行漏匹配 → 契约集不完整；基线仅含最早 runid，无防"基线被污染后漂移"快照 |

> 归纳：RR 机制**能**抓到真实缺陷（它定位了 K1），但当前在"golden / 逐行契约"粒度上是**不可信 oracle**——
> 健康内核在 icount 并发日志下约 1/2 冷启产生撕裂行，直接把确定性(A)/复原(D)翻成 "BUG?" 假红。
> 与 test-tr 的 `l[ls] /:` flake 同源（同为串口字符级交织）。**修复依赖 K1，且辅以 tr_mark 窗口+撕裂归一化。**

### R5 共享 build/ 竞态（确认）
- `build/transcripts` 与 `TR_LOG` 均住在 `build/` 内；`make clean`（Makefile L274）`rm -rf build/`。
- 任意并发重负荷 harness（如 test-tcp-attack 的 `make clean && make TCP_DEMO=1`、test-slip）都会把**另一并发 harness 的存活 `build/transcripts/<runid>/` 与日志现场直接抹掉** → 假 ack 超时/金标丢失。
- 与 RR 交接档自述一致（已知项）；修复落点：`BUILD_DIR`/`TR_BASE` 隔离到每 harness 私有目录。

## 阶段3-5：T1 攻击实测汇总
| 面 | 载荷 | 实测结果 |
|---|---|---|
| 内存/进程/FS/shell/ELF | 基线 make test 覆盖（isol/forkdemo/stackovf/deep/deepfork/deepexec/heapdemo/bigdemo/abuse/fsdemo/waitdemo/selftest） | ✅ 全绿，无内核标记（abuse 17 越权、stackovf guard 击杀、deepexec 栈 exec、fs crash-quarantine 均按预期） |
| 网络 TCP | test-tcp-attack：3738 脏注入包 / 3 线程 / ~25s，业务 8KB+TAIL | ✅ RESULT PASS、无副作用、无异常标记 |
| 网络(SLIP/DHCP/UDP/ICMP/ARP) | 基线 test-slip/test-net/test-socket | ✅ 全绿 |
| cc500 边界 | `writefile` `/u.c int main(int x`、`char*s="abc`、`return undef();`、`/*` | ✅ `/u.c` 干净 `cc500: error at` + `compile FAIL code=1`，无 hang 无崩溃；`/s.c` 子项因 **K1 撕裂导致 ack 握手失步**而判定为"不可用"（非内核挂起，见下） |
| shell 特殊/超长 | `ls /<600x>`、`rm /..`、`mkdir //`、`cat /`、`run`(空参) | 注入中断于 K1 引发的握手失步；未取得完整结论 |

> **K1 影响面补强**：edge 会话中 shell 回显行 `ccrun /u.c /u.elf` 被撕裂为
> `n /u.c /u.[sched] wake pid=1 at tick=...`（edge_atk.log L1022）——命令回显也会被并发 sched 打印切开。
> 这使基于 ack 计数同步的外部 agent 握手失步（后续命令 readline 计数漂移），即 K1 的破坏超过"日志污染"，
> 直接干扰 RR/AI-agent 输入同步协议。

## 阶段6：结论（待办/gate 建议）
见交付回复。核心：**K1（serial_printf 无 IRQ 原子性）是当前 RR 判定失真的总根因**；修复后 test-tr/rp_torture 的
确定性/复原判据应复测转稳定。