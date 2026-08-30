/* mini-os/v2-c-kernel/version.h
 * 版本单一来源（独立评估 L-4）：内核启动横幅 / shell banner / initramfs motd /
 * 回归断言统一取这里的 MINI_OS_VERSION——发布新版本只需改这一处，不再多处漂移。
 * 使用方：#include "version.h" 后以字符串拼接 `"... " MINI_OS_VERSION " ..."`。 */
#ifndef _VERSION_H
#define _VERSION_H

#define MINI_OS_VERSION "v0.30"

#endif
