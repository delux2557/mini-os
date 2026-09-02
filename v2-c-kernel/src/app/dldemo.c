/* mini-os/v2-c-kernel/src/app/dldemo.c
 * 虚拟 TCP · 大文件下载演示（netif Step 4 / v1.2 扩展）。
 * 与 httpdemo.c 不同：前者把整份响应攒进 static char resp[TCP_RXB+1]（16KB），
 * 验证的是">旧 TCP_RXB(4096)"这个量级；本 demo 验证「大文件下载无总字节上限」——
 *   1) 每轮 tcp_recv 只拿一小块（rxb[1024]），边收边累加进一个大静态缓冲 dload[DL_MAX]；
 *   2) 不同长 16KB 上限，把总长度推到 128KB（DL_MAX=131072），证明环/分块/背压都能扛；
 *   3) 分三给校验：状态行含 "200 OK"、总长度 == 期望 DL_EXPECT、尾部固定标记 "EOFTAIL" 完整。
 * 输出：一次 sys_print 原子打印里程碑（多进程并发无锁），末尾 RESULT PASS/FAIL。
 */
#include <stdint.h>
#include "user_lib.h"
#include "tcp.h"

#define DL_IP     0x7F000001u   /* 127.0.0.1：宿主大文件服务（转发器视角的真实可达目标） */
#define DL_PORT   8080
#define DL_MAX    131200        /* 捕获上限：HTTP 头(~55B) + 131072B body，须 ≥ 头+body 总量，
                                   使 body 末尾 EOFTAIL 落在捕获窗口内 */
#define DL_EXPECT 131072        /* 期望的 body 大小（128KB） */
#define DL_EOFTAIL "EOFTAIL"   /* 128KB body 末尾固定 7 字节标记，校验尾部是否完整 */

static char lbuf[160];
static uint32_t llen;
static void ap(const char *s) { while (*s && llen < sizeof(lbuf) - 1) lbuf[llen++] = *s++; }
static void apn(uint32_t n) {
    char t[12]; int i = 0;
    if (n == 0) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i) lbuf[llen++] = t[--i];
}
static void line(void) { lbuf[llen] = '\n'; lbuf[llen + 1] = 0; sys_print(lbuf); llen = 0; }

static int has_substr(const char *h, const char *needle) {
    if (!h || !*h) return 0;
    for (; *h; h++) {
        const char *p = h, *q = needle;
        while (*q && *p && *p == *q) { p++; q++; }
        if (!*q) return 1;
    }
    return 0;
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    llen = 0;
    ap("[http] DL demo pid="); apn(sys_getpid()); line();

    int fd = tcp_open(DL_IP, DL_PORT);
    if (fd < 0) { ap("[http] DL tcp_open FAIL"); line(); return; }
    ap("[http] DL tcp_open -> fd="); apn((uint32_t)fd); line();
    if (tcp_wait_open(fd) < 0) { ap("[http] DL connect ERR"); line(); return; }

    static const char req[] = "GET /bigup HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n";
    int sn = tcp_send(fd, (const uint8_t *)req, (uint32_t)(sizeof(req) - 1));
    ap("[http] DL tcp_send -> "); apn((uint32_t)sn); ap("B"); line();
    if (sn < 0) { ap("[http] DL send FAIL"); line(); return; }

    /* 边 tcp_recv 边累加：不复用固定 TCP_RXB 缓冲，总长一路推到 128KB。
     * 读缓冲 ≥ 单数据报载荷(TCP_MAX_PAYLOAD=1392)，否则每轮只拷出 1024、留 368 在
     * TCP_RXB 环里越积越多，44 轮后环满丢字节（v1.2 实测：rxb[1024] 只剩 118KB）。 */
    static char dload[DL_MAX];         /* static 避免 128KB 压栈 */
    uint8_t rxb[2048];
    uint32_t rlen = 0;
    int rc, got_200 = 0, closed = 0;
    for (int round = 0; round < 400 && !closed; round++) {   /* 400×2KB 足够 128KB + 容错 */
        rc = tcp_recv(fd, rxb, sizeof(rxb));
        if (rc > 0) {
            for (int i = 0; i < rc && rlen < DL_MAX; i++) dload[rlen++] = (char)rxb[i];
            dload[rlen] = 0;
            if (has_substr(dload, "200 OK")) got_200 = 1;
        } else if (rc == 0) {
            closed = 1;
        } else {
            ap("[http] DL recv ERR (unexpected)"); line();
            break;
        }
    }
    dload[rlen] = 0;

    /* 剥 HTTP 头：第一个 "\r\n\r\n" 之后才是 body。EOFTAIL 在 body 末尾，
     * 头字节若计入会把它挤出截断窗口（v1.2 实测 tail=MISS）。 */
    uint32_t hdr_off = 0;
    for (uint32_t i = 0; i + 3 < rlen; i++) {
        if (dload[i] == '\r' && dload[i + 1] == '\n' &&
            dload[i + 2] == '\r' && dload[i + 3] == '\n') { hdr_off = i + 4; break; }
    }
    const char *body = dload + hdr_off;
    uint32_t blen = (rlen > hdr_off) ? (rlen - hdr_off) : 0;

    /* 尾部完整性：body 长度 128KB 后，末尾 7 字节 == "EOFTAIL" 才完整 */
    static const char *et = DL_EOFTAIL;
    int etlen = 0; while (et[etlen]) etlen++;
    int tail_ok = 0;
    if (blen >= (uint32_t)etlen) {
        tail_ok = 1;
        for (int i = 0; i < etlen; i++)
            if (body[blen - (uint32_t)etlen + (uint32_t)i] != et[i]) { tail_ok = 0; break; }
    }
    ap("[http] DL HTTP "); ap(got_200 ? "200 OK" : "NO-200"); ap(" closed=");
    apn(closed ? 1u : 0u); ap(" len="); apn(blen); ap("/"); apn(DL_EXPECT);
    ap(" tail="); ap(tail_ok ? "EOFTAIL" : "MISS"); line();

    int ok1 = (got_200 && closed && tail_ok && blen == DL_EXPECT);
    tcp_close(fd);

    ap("[http] DL RESULT "); ap(ok1 ? "PASS" : "FAIL");
    ap(" (len="); apn(blen); ap("/"); apn(DL_EXPECT); ap(",tail=");
    ap(tail_ok ? "ok" : "miss"); ap(")"); line();
}