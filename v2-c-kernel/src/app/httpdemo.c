/* mini-os/v2-c-kernel/src/app/httpdemo.c
 * 虚拟 TCP 薄包装演示（netif Step 4）：经"宿主转发器 -> 真实 TCP"拉一次 HTTP 请求-响应。
 *  阶段1（成功）：tcp_open(127.0.0.1:8080) -> tcp_send(GET) -> 循环 tcp_recv
 *    -> 收到含 "200 OK" 的响应体，对端 `Connection: close` 正常关闭时 recv 返回 0
 *  阶段2（失败路径断言）：tcp_open 到无监听端口 -> 转发器连接被拒回 MSG_ERROR
 *    -> tcp_recv 返回 -1（"失败"与"对端关闭 0"必须可区分，见 docs/tcp-thin-api.md §1.1）
 * 输出每条里程碑用"一次" sys_print 原子打印（多进程并发无锁，拆多次会撕裂一行）。 */
#include <stdint.h>
#include "user_lib.h"
#include "tcp.h"

#define HTTP_IP     0x7F000001u   /* 127.0.0.1：宿主 HTTP 服务（以转发器视角的真实可达目标） */
#define HTTP_PORT   8080
#define REFUSE_PORT 59998         /* 宿主上无监听 -> 测 MSG_ERROR / -1 路径 */

/* 用户态无 libc strstr，手写子串匹配（判断响应状态行含 "200 OK"） */
static int has_substr(const char *h, const char *needle) {
    if (!h || !*h) return 0;
    for (; *h; h++) {
        const char *p = h, *q = needle;
        while (*q && *p && *p == *q) { p++; q++; }
        if (!*q) return 1;
    }
    return 0;
}

static char lbuf[128];
static uint32_t llen;
static void ap(const char *s) { while (*s && llen < sizeof(lbuf) - 1) lbuf[llen++] = *s++; }
static void apn(uint32_t n) {
    char t[12]; int i = 0;
    if (n == 0) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i) lbuf[llen++] = t[--i];
}
static void line(void) { lbuf[llen] = '\n'; lbuf[llen + 1] = 0; sys_print(lbuf); llen = 0; }

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    llen = 0;
    ap("[http] virtual TCP demo pid="); apn(sys_getpid()); line();

    /* ---- 阶段1：成功路径 ---- */
    int fd = tcp_open(HTTP_IP, HTTP_PORT);
    if (fd < 0) { ap("[http] tcp_open FAIL"); line(); return; }
    ap("[http] tcp_open -> fd="); apn((uint32_t)fd); line();

    /* 等待转发器与目标建立真实 TCP（OPENED 事件）；超时/被拒 -> tcp_wait_open 返回 -1 */
    int wo = tcp_wait_open(fd);
    ap("[http] wait_open -> "); apn((uint32_t)wo); line();
    if (wo < 0) { ap("[http] connect ERR"); line(); return; }

    static const char req[] = "GET / HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n";
    int sn = tcp_send(fd, (const uint8_t *)req, (uint32_t)(sizeof(req) - 1));
    ap("[http] tcp_send -> "); apn((uint32_t)sn); ap("B"); line();
    if (sn < 0) { ap("[http] HTTP send FAIL"); line(); return; }

    uint8_t rxb[512];
    char resp[640]; uint32_t rlen = 0;
    int rc, got_200 = 0, closed = 0;
    for (int round = 0; round < 30 && !closed; round++) {
        rc = tcp_recv(fd, rxb, sizeof(rxb));
        if (rc > 0) {
            for (int i = 0; i < rc && rlen < sizeof(resp) - 1; i++) resp[rlen++] = (char)rxb[i];
            resp[rlen] = 0;
            if (has_substr(resp, "200 OK")) got_200 = 1;
            ap("[http] recv +"); apn((uint32_t)rc); ap("B"); line();
        } else if (rc == 0) {
            closed = 1;                     /* 对端正常关闭 */
        } else {
            ap("[http] recv ERR (unexpected)"); line();
            break;
        }
    }
    ap("[http] HTTP "); ap(got_200 ? "200 OK" : "NO-200"); ap(" closed=");
    apn(closed ? 1u : 0u); line();
    int ok1 = (got_200 && closed);
    tcp_close(fd);

    /* ---- 阶段2：失败路径（连接被拒 -> -1，与 0 可区分） ---- */
    int fd2 = tcp_open(HTTP_IP, REFUSE_PORT);
    int rc2 = -2;
    if (fd2 >= 0) {
        rc2 = tcp_recv(fd2, rxb, sizeof(rxb));   /* 转发器回 MSG_ERROR -> -1 */
        tcp_close(fd2);
    }
    ap("[http] refuse recv -> "); ap((rc2 < 0) ? "-1" : "?"); line();
    int ok2 = (rc2 == -1);

    ap("[http] RESULT "); ap((ok1 && ok2) ? "PASS" : "FAIL");
    ap(" (http="); ap(ok1 ? "ok" : "bad"); ap(",refuse="); ap(ok2 ? "-1" : "?"); ap(")");
    line();
}