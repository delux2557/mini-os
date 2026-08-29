/* mini-os/v2-c-kernel/src/app/sockdemo.c
 * v0.20 用户态 UDP socket 演示：经 SLIRP 网关与宿主 UDP echo 服务
 * （127.0.0.1:7777，由 tests/test_net.sh 提供）完成 发 PING -> 收 PONG 的
 * 端到端回环，验证 sys_net_socket/sendto/recvfrom/close 系统调用全链路。
 * 注意：recvfrom 是非阻塞的（与轮询式 e1000 驱动对齐），这里轮询+让出。
 * 输出：每条里程碑用"一次" sys_print 原子打印（多进程并发写串口无锁，
 * 拆成多次 sys_print 会被其它进程的日志插入，破坏一行完整性）。 */
#include "user_lib.h"

#define GW_IP     0x0A000202u   /* 10.0.2.2：SLIRP 网关（宿主 127.0.0.1 别名） */
#define ECHO_PORT 7777

/* ---- 行缓冲格式化：拼完一整行后单次 sys_print ---- */
static char lbuf[96];
static uint32_t llen;

static void ap(const char *s) { while (*s && llen < sizeof(lbuf) - 1) lbuf[llen++] = *s++; }
static void apdec(uint32_t n) {
    char t[12]; int i = 0;
    if (n == 0) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i) lbuf[llen++] = t[--i];
}
static void line_end(void) {
    lbuf[llen] = '\n';
    lbuf[llen + 1] = 0;
    sys_print(lbuf);
    llen = 0;
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    llen = 0;
    ap("[sock] udp socket demo pid="); apdec(sys_getpid()); line_end();

    int s = sys_net_socket(0);          /* 本地端口自动分配 */
    if (s < 0) { sys_print("[sock] socket() FAIL\n"); return; }
    ap("[sock] socket -> id="); apdec((uint32_t)s); line_end();

    int ok = 0;
    for (int attempt = 0; attempt < 6 && !ok; attempt++) {
        struct net_send_iov si;
        si.dst_ip = GW_IP;
        si.dst_port = ECHO_PORT;
        si.buf = (const uint8_t *)"PING";
        si.len = 4;
        int n = sys_net_sendto(s, &si);
        ap("[sock] sendto PING -> "); apdec((uint32_t)n); ap("B"); line_end();
        if (n != 4) break;

        for (int i = 0; i < 2000 && !ok; i++) {
            uint8_t rxb[64];
            struct net_recv_iov ri;
            ri.buf = rxb;
            ri.max = sizeof(rxb);
            ri.src_ip = 0;
            ri.src_port = 0;
            int m = sys_net_recvfrom(s, &ri);
            if (m >= 4 && rxb[0] == 'P' && rxb[1] == 'O' &&
                rxb[2] == 'N' && rxb[3] == 'G') {
                ap("[sock] recvfrom PONG +"); apdec((uint32_t)(m - 4));
                ap("B from "); apdec(ri.src_ip & 0xFF); ap(".");
                apdec((ri.src_ip >> 8) & 0xFF); ap(".");
                apdec((ri.src_ip >> 16) & 0xFF); ap(".");
                apdec((ri.src_ip >> 24) & 0xFF); ap(":");
                apdec(ri.src_port); line_end();
                ok = 1;
                break;
            }
            sys_sleep(1);               /* 1 tick 后再试，让包有时间回来 */
        }
    }
    sys_print(ok ? "[sock] UDP round-trip OK\n" : "[sock] UDP round-trip FAIL\n");
    sys_net_close(s);
}
