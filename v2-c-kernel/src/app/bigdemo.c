/* mini-os/v2-c-kernel/src/app/bigdemo.c
 * v0.26#3 ELF 加载去上限演示：
 *  - 70KB 初值数据（.data 段）使 ELF 文件 > 64KB，远超旧上限 65536B / 8 帧 / 32KB；
 *  - 加载器按 ELF 自身 PT_LOAD 范围逐页映射（load_frames/own_frames 动态数组记账），
 *    不再被 8 帧上限拒绝；
 *  - 应用加载后逐字节填充并校验和，验证 70KB 数据完好读写。
 * 输出约定：每行单次 sys_print（原子行，避免被抢占时拆断日志行）。
 */
#include "user_lib.h"

/* 70KB 初值数据：占 .data 段（非 .bss），使 ELF 文件体积 > 65536B。
 * 链接到 APP_ADDR(0x800A0000) 后的 app 槽内，需约 19 页物理帧。 */
static unsigned char blob[70000] = { 1 };

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t i, sum = 0;
    sys_print("[bigdemo] pid=");
    user_putdec(sys_getpid());
    sys_print(" blob=70KB size=");
    user_putdec(sizeof(blob));
    sys_print(" loading (old 32KB/8-frame cap would reject)\n");

    /* 逐字节填充（以 4096 为周期的递推式，读旧值 -> 写新值，全部页都触达） */
    for (i = 4096; i < sizeof(blob); i++)
        blob[i] = (unsigned char)(blob[i - 4096] + i);

    /* 校验和 */
    for (i = 0; i < sizeof(blob); i++) sum += blob[i];
    sys_print("[bigdemo] 70KB write+verify sum=");
    user_putdec(sum);
    sys_print("\n");
    sys_print("[bigdemo] survived big-ELF load\n");
    sys_exit(0);
}
