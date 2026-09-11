/* mini-os/v2-c-kernel/src/version.h
 * 版本单一来源（独立评估 L-4）：内核启动横幅 / shell banner / initramfs motd /
 * 回归断言统一取这里的 MINI_OS_VERSION——发布新版本只需改这一处，不再多处漂移。
 * 使用方：#include "version.h" 后以字符串拼接 `"... " MINI_OS_VERSION " ..."`。
 * v1.5（文档整改）：版本串自 v0.33 并入 v1.x 线，与 changelog 唯一版本事实源对齐
 * （changelog 自 v1.1 起已进入 v1.x 线，banner 此前滞后于代码注释的 v0.34~v0.38）。 */
#ifndef _VERSION_H
#define _VERSION_H

#define MINI_OS_VERSION "v1.5"

#endif
