/* mini-os/v2-c-kernel/src/drv/uart_netif.h
 * COM2 串口网卡（路线图 v1.1 Step 2）：SLIP（RFC 1055）封装，与 e1000 平级的第二个
 * netif 后端（D1：IP 数据报 over 串口；COM1 被 shell 终端占用，本网卡只用 COM2=0x2F8）。
 * 注册后由 netif_init_all 按其优先级选择为当前网卡（正常 e1000 优先；测试用
 * UART_NETIF_DEFAULT 使串口优先，静态绑定，D6）。
 */
#ifndef DRV_UART_NETIF_H
#define DRV_UART_NETIF_H

/* 把 uart_netif 注册进 netif 注册表（协议/系统代码不直接调用 uart_* 收发，只走 netif） */
void uart_netif_register(void);

#endif /* DRV_UART_NETIF_H */