/* mini-os/v2-c-kernel/src/apps/waitdemo.c
 * v0.15 wait 语义演示：经典 wait()（等待任意子进程）+ waitpid(pid)。
 *  - 父进程 fork 出 3 个子进程，各自 sys_exit(7/9/11)
 *  - 父进程循环 sys_wait(-1, &code)：每次回收"任意一个"已退出的子进程，
 *    返回其 pid、退出码写 *status；收集齐 3 个后，再一次 wait(-1) 应返回 -1
 *  - 校验：3 个 pid 各不相同、3 个退出码恰为 {7,9,11}
 * 输出约定：每行单次 sys_print（原子行），避免被抢占时其它进程输出拆断日志行。
 */
#include "user_lib.h"

static void putdec_buf(char *buf, uint32_t *i, uint32_t v) {
    char tmp[12];
    int j = 0;
    if (v == 0) tmp[j++] = '0';
    while (v) { tmp[j++] = (char)('0' + (v % 10)); v /= 10; }
    while (j && *i < 90) buf[(*i)++] = tmp[--j];
}
static void catstr(char *buf, uint32_t *i, const char *s) {
    while (*s && *i < 90) buf[(*i)++] = *s++;
}
static void emit_line(char *buf, uint32_t i) {
    buf[i] = '\n';
    buf[i + 1] = 0;
    sys_print(buf);
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[96];
    uint32_t i;
    sys_print("[waitdemo] v0.15 wait() any-child demo\n");

    /* fork 3 个子进程，各自不同退出码 */
    uint32_t c1 = sys_fork();
    if (c1 == 0) {
        i = 0; catstr(buf, &i, "[waitdemo] child A pid=");
        putdec_buf(buf, &i, sys_getpid()); catstr(buf, &i, " exit 7");
        emit_line(buf, i);
        sys_exit(7);
    }
    uint32_t c2 = sys_fork();
    if (c2 == 0) {
        i = 0; catstr(buf, &i, "[waitdemo] child B pid=");
        putdec_buf(buf, &i, sys_getpid()); catstr(buf, &i, " exit 9");
        emit_line(buf, i);
        sys_exit(9);
    }
    uint32_t c3 = sys_fork();
    if (c3 == 0) {
        i = 0; catstr(buf, &i, "[waitdemo] child C pid=");
        putdec_buf(buf, &i, sys_getpid()); catstr(buf, &i, " exit 11");
        emit_line(buf, i);
        sys_exit(11);
    }

    /* 父进程 */
    i = 0; catstr(buf, &i, "[waitdemo] parent pid=");
    putdec_buf(buf, &i, sys_getpid());
    catstr(buf, &i, " forked "); putdec_buf(buf, &i, c1);
    catstr(buf, &i, ","); putdec_buf(buf, &i, c2);
    catstr(buf, &i, ","); putdec_buf(buf, &i, c3);
    emit_line(buf, i);

    /* 循环 wait(-1)：回收任意子进程，直到无子进程 */
    int kids[3], codes[3], n = 0;
    while (n < 3) {
        int code = -1;
        int rpid = sys_wait((uint32_t)-1, &code);
        if (rpid <= 0) break;
        kids[n] = rpid; codes[n] = code; n++;
        i = 0; catstr(buf, &i, "[waitdemo] wait any -> pid=");
        putdec_buf(buf, &i, (uint32_t)rpid);
        catstr(buf, &i, " code=");
        putdec_buf(buf, &i, (uint32_t)code);
        emit_line(buf, i);
    }

    /* 校验：回收数量 / pid 互异 / 退出码集合 {7,9,11} */
    int ok = (n == 3);
    int seen7 = 0, seen9 = 0, seen11 = 0;
    for (int k = 0; k < n; k++) {
        if (codes[k] == 7) seen7 = 1;
        if (codes[k] == 9) seen9 = 1;
        if (codes[k] == 11) seen11 = 1;
        for (int m = k + 1; m < n; m++)
            if (kids[k] == kids[m]) ok = 0;   /* pid 不得重复 */
    }
    if (!(seen7 && seen9 && seen11)) ok = 0;
    sys_print(ok ? "[waitdemo] verify OK (3 kids, codes 7/9/11)\n"
                 : "[waitdemo] verify FAIL\n");

    /* 全部回收后再 wait(-1) 应返回 -1 */
    int code = -1;
    int final = sys_wait((uint32_t)-1, &code);
    i = 0; catstr(buf, &i, "[waitdemo] final wait any -> ");
    putdec_buf(buf, &i, (uint32_t)final);
    catstr(buf, &i, " (expect -1)");
    emit_line(buf, i);

    sys_print("[waitdemo] done\n");
}
