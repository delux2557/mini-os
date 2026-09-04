/* mini-os/v2-c-kernel/tests/host_serial_stub.c
 * 宿主单测的 serial_printf 桩：OBS-R1 起 kb.c 的丢弃告警会调 serial_printf，
 * 宿主 test_kb 无串口硬件（serial.c 写 0x3F8 端口），此处空转以满足链接。
 * 签名与 src/drv/serial.h 声明一致（void serial_printf(const char *, ...)）。
 * 仅供宿主编译，不参与内核链接（内核用真实 serial.c）。 */
void serial_printf(const char *fmt, ...) {
    (void)fmt;   /* 宿主测试不关心丢弃计数文本，空转 */
}