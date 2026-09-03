# mini-os v2-c-kernel FS（文件系统）子系统专项深审报告

> **来源**：代码审查专家（本轮专项审计）
> **审计对象**：`v2-c-kernel/src/fs/`（fs.c 455 行 / storage.c / blockdev）+ `src/drv/ata.c`（后端）
> **方法**：逐行读码 + 布局/路径解析/资源释放路径推演 + 与 BUG-015/031 对照。
> **边界**：纯静态读码，未动态运行；结论为架构/正确性评估。

---

## 0. 结论

**纯逻辑复合型**极简 FS（超级块 + inode 位图 + 数据位图 + 直接/间接块 + 目录层级），设计原则正确
（全程只经 `blockdev` 抽象，可在宿主单测；`src/fs` 无内核/调度依赖）。路径解析 `fs_walk` 三态语义、
间接块惰性分配、删除/rmdir 资源释放完整。**无 P1 缺陷**，登记 `OBS-FS-x`（2×P2 + 3×P3）。

---

## 1. 对象与边界

- 布局：块 4KB；`super|inode位图|data位图|inode表(64×64B)|数据块(252)`；存储层 `storage.c` 把
  1MB ramdisk（frame_alloc_run 连续帧）作主，磁盘可整盘镜像持久化（`storage_sync` 全量写回）。
- 方法：静态读码 + 路径/释放/扩容路径推演；未动态运行。

## 2. 架构正向核实

- **blockdev 抽象成立**：`fs_read/write` 全走 `block_Read_write`，`blockdev_ptr` 直接映射地址；
  配套 `test_fs.c` 宿主单测（纯逻辑无 QEMU）。
- **三态路径语义清晰**：`fs_walk` 返回「叶子存在(>=0)/叶子缺失(-1 且 leaf 已填)/中间组件不存在或非目录
  (-1 且 leaf 空)」；`fs_lookup` 使 "路径即目录" 与 "叶子缺失" 可区分——调用方复用不误判。
- **资源释放闭环**：`free_inode_blocks`（直接+间接块+间接块本身）→ `fs_delete`/`fs_rmdir` 均归还
  inode 位图 + 删目录项；`fs_make` 的 `dir_add` 失败会回滚已分配 inode（[fs.c:262-264](file:///workspace/mini-os/v2-c-kernel/src/fs/fs.c#L262-L264)）。
- **惰性块分配 + 短写可控**：`file_block(create=1)` 首次写才分配；`fs_write` 预分配失败降级为短写并更新
  `in.size`（[fs.c:399-405](file:///workspace/mini-os/v2-c-kernel/src/fs/fs.c#L399-L405)），无半写无 inode 破坏。

## 3. 发现项（`OBS-FS-*`）

### OBS-FS-1【P2】目录条目不支持间接块扩容（目录容量硬绑 FS_DIRECT_BLOCKS）
`dir_add` 扩容循环只到 `FS_DIRECT_BLOCKS`（12 直接块，[fs.c:88](file:///workspace/mini-os/v2-c-kernel/src/fs/fs.c#L88)），
单目录条目上限 ≈ 12×128=1536。文件数据走间接块（单文件可达 12+1024 块），而**目录无间接块**——
若 initramfs 或用户创建条目超过 1536，`dir_add` 返回 -1，`fs_make` 静默失败。当前 initramfs ~25 文件远未触顶，
但用户态高频创建会撞上限且无显式错误。建议：目录走 `file_block` 同样的直接+间接路径，或至少在
`dir_add` 返回 -1 时向调用方（`fs_make`）透出"目录已满"而非静默。

### OBS-FS-2【P2】磁盘持久化全量同步无脏追踪（3248 扇区逐扇区）
`disk_save`（[storage.c:43-56](file:///workspace/mini-os/v2-c-kernel/src/fs/storage.c#L43-L56)）把 1MB ramdisk
**无条件全量逐扇区**写回 ATA。单扇区暂存（`sector_buf`）1024×? 次同步 I/O，`shell save`/`sys_fs_sync`
均走此——演示可用但随文件增长越来越慢，且反复 save 重复写未变扇区。P2 建议：加「脏块位图」（仅写
ramdisk 与盘不一致的扇区），并给 sync 打印实际写回扇区数。

### OBS-FS-3【P3】`bitmap_test/set` 对 `bit >= nbits` 无越界护栏
`bitmap_alloc` 保证 `i<nbits`，`dir_add` 的间接路径未复用 bitmap 越界——但 `free_inode_blocks` 的
`ptrs[k]` 范围依赖 `FS_INDIRECT_BLOCKS`；`bitmap_set(-data blk)` 直接以块号索引，无「块号 < bd->blocks」
校验。当前布局安全（块号恒在数据区），但分层防御建议：`bitmap_set/test` 加 `bit < nbits` 断言。

### OBS-FS-4【P3】`fs_read` 依赖 `in.size` 而非 `FS_MAX_FILE_SIZE` 双界
`fs_read` 用 `off>=in.size return 0` 早退后 clamp（[fs.c:369-370](file:///workspace/mini-os/v2-c-kernel/src/fs/fs.c#L369-L370)），
与 `fs_write` 的 `FS_MAX_FILE_SIZE` clamp 双界不对称；`fs_read` 对 `in.size == FS_MAX_FILE_SIZE` 时的
`off` 上限依赖 inode 一致性。P3 观察：建议 `fs_read` 也统一 `FS_MAX_FILE_SIZE` clamp，避免 inode size
被破坏时读越界。

### OBS-FS-5【P3】`storage` 整盘读写均无中途失败回滚
`disk_load`/`disk_save` 若 ATA 中途失败（`ata_read/write_sectors` 返回 -1）即 `break`，ramdisk 与盘
进入不一致状态；`storage_sync` 返回 -1 但已写了部分。对"断电安全"不承诺的教学 FS 可接受，建议在
`storage_sync` 注释显式声明"非原子、断电可能半写"，并建议调用方（shell save 后）复核返回。

## 4. 与历史 bug 对照

| 历史 | 现况 | 状态 |
|---|---|---|
| BUG-015 fs_walk 失败未写 leaf/dirout | v0.14 三态输出已修 | 已封堵 |
| BUG-031 全局文件槽泄漏（cc500 相关） | **v0.31 per-process fd** 根治 | 已封堵（跨子系统） |

## 5. 建议排期

| 项 | 优先级 | 类型 | 建议 |
|---|---|---|---|
| OBS-FS-1 | P2 | 能力 | 目录间接块扩容或显式"目录满"错误 |
| OBS-FS-2 | P2 | 性能 | 脏块位图 + sync 只写不一致扇区 |
| OBS-FS-3 | P3 | 防御 | bitmap 越界护栏/断言 |
| OBS-FS-4 | P3 | 一致性 | fs_read 统一 FS_MAX_FILE_SIZE clamp |
| OBS-FS-5 | P3 | 文档 | storage_sync 非原子语义显式声明 |

*注：全项为静态推演的观察/加固，无已证崩溃；当前 initramfs 与演示流量远未触顶 OBS-FS-1 上限。*