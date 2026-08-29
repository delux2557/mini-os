/* mini-os/v2-c-kernel/src/storage.h
 * 存储子系统（v0.16）：ramdisk 块设备 + ATA 真盘持久化。
 *  - storage_init()：探测 ATA -> 整盘读入 ramdisk -> 有有效 FS 则挂载，
 *    否则格式化 + 写入 initramfs（首启并落盘一次）
 *  - storage_sync()：把 ramdisk 全量写回磁盘（sys_fs_sync / shell save）
 */
#ifndef _STORAGE_H
#define _STORAGE_H

void storage_init(void);
int  storage_sync(void);   /* 0=已保存（有盘），-1=无盘可存 */

#endif
