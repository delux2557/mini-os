/* mini-os/v2-c-kernel/src/drv/kb.c
 * PS/2 键盘驱动（scan code set 1）：
 *  IRQ1 中断里读扫描码 -> 查表转 ASCII -> 放入环形缓冲
 *  主循环通过 kb_getchar() 轮询读取 */
#include "kb.h"
#include "idt.h"
#include "serial.h"   /* OBS-R1: 非 ASCII 丢弃计数告警走既有 serial_printf */
#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* 普通键位（无 Shift），scan code set 1，共 128 项 */
static const char sc_ascii[128] = {
    0, 27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\','z',
    'x','c','v','b','n','m',',','.','/', 0,'*', 0,' ', 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* Shift 组合键位 */
static const char sc_shift[128] = {
    0, 27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|','Z',
    'X','C','V','B','N','M','<','>','?', 0,'*', 0,' ', 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

#define KB_BUF 256
static char buf[KB_BUF];
static volatile int head = 0, tail = 0;
static int shift_down = 0;

/* ---- v0.9 行缓冲 + v0.37 挂起行队列：把按键组装成"一行"，供阻塞式 sys_readline 使用 ----
 * 退格删行尾、回车定行；行完成时调用回调（供内核唤醒阻塞的 readline 进程）。
 * v0.37（BUG-075）：定行即把整行复制入"挂起队列"并立即让位组装下一行——消除
 * 旧实现 line_ready=1 到 kb_line_take 之间"所有输入被丢弃"的窗口（多行粘贴/heredoc
 * 一次只进一行、只能分批）。组装行"定行即清空"保留 v0.30 防两行合并的本质（不会串行）。
 * 行缓冲与字符环形缓冲独立：字符缓冲仍供 idle 实时回显。 */
static char line_buf[KB_LINE_MAX + 1];        /* 当前组装行（定行即入队、清空让位） */
static int  line_len = 0;
static kb_line_hook_t line_hook = 0;
static char line_q[KB_LINE_Q][KB_LINE_MAX + 1];  /* 挂起行队列（环形，NUL 结尾） */
static int  q_head = 0;                       /* 队头（下一行被取走的槽） */
static int  q_cnt = 0;                        /* 待取行数 */

/* ---- OBS-R1：串口/kb 非 ASCII 高位字节丢弃计数告警 ----
 * kb_feed_char 以 char 收单字节，≥0x80 经符号位扩展为负、被 `c>=32` 判定静默丢弃，
 * 实测非 ASCII 载荷/路径经串口注入逐字节丢失且无任何可见告警。此处对每次非 ASCII 丢弃
 * 累计计数，行结束（定行/取行）且计数>0 时打印一行 `[kb] N non-ascii bytes dropped`。
 * 对纯 ASCII 回归零影响（计数恒 0、零新输出）；仅行内计数告警，不做转义/UTF-8 全支持
 * （如需另立项）。 */
static uint32_t kb_nonascii_dropped = 0;

static void kb_report_drops(void) {
    if (kb_nonascii_dropped) {
        serial_printf("[kb] %u non-ascii bytes dropped\n", kb_nonascii_dropped);
        kb_nonascii_dropped = 0;
    }
}

/* ---- F10/task3b：行溢出（静默截断）告警 ----
 * readline 行缓冲上限 KB_LINE_MAX=128：可打印字符在行已满仍被丢弃 -> 整行在 128B 处被
 * 静默截断，外部 agent 源码写入会莫名失败（如 micc input open fail）。与 OBS-R1 同款：
 * 只在"行已满再收到可打印字符"置位、行结束打印一次提示，对纯短行零影响（无新输出）。 */
static int kb_line_overlong = 0;
static void kb_report_overlong(void) {
    if (kb_line_overlong) {
        serial_printf("[kb] warning: input line >%dB truncated; use writefile <<DELIM heredoc for long content\n",
                      KB_LINE_MAX);
        kb_line_overlong = 0;
    }
}

/* v0.37：挂起行队列满（>KB_LINE_Q 行未取走）——最老行保留、新定行丢弃，取行时补报一次 */
static int kb_q_overflow = 0;
static void kb_report_qfull(void) {
    if (kb_q_overflow) {
        serial_printf("[kb] warning: pending-line queue full (%d), a line dropped; paste in smaller chunks\n",
                      KB_LINE_Q);
        kb_q_overflow = 0;
    }
}

static void kb_cb(registers_t *r) {
    (void)r;
    kb_feed_scan(inb(0x60));
}

void kb_set_line_hook(kb_line_hook_t fn) { line_hook = fn; }

void kb_line_reset(void) { line_len = 0; q_head = 0; q_cnt = 0; kb_line_overlong = 0; kb_q_overflow = 0; }

int kb_line_ready(void) { return q_cnt > 0; }

int kb_line_take(char *out, uint32_t max) {
    if (q_cnt <= 0) return -1;
    uint32_t n = 0;
    while (n < KB_LINE_MAX && line_q[q_head][n]) n++;   /* 队头行长（NUL 结尾） */
    if (n >= max) n = max - 1;
    uint32_t i;
    for (i = 0; i < n; i++) out[i] = line_q[q_head][i];
    out[n] = 0;
    q_head = (q_head + 1) % KB_LINE_Q;
    q_cnt = q_cnt - 1;
    kb_report_drops();   /* OBS-R1: 取行后同样兜底上报（换行上报已重置，此处恒零或补报） */
    kb_report_overlong(); /* F10: 取行后兜底上报截断提示（同 OBS-R1，已重置则恒零） */
    kb_report_qfull();   /* v0.37: 队列满丢行补报（已重置则恒零） */
    return (int)n;
}

/* 处理一个已解析的 ASCII 字符（键盘查表结果或串口注入）：
 *  行缓冲组装：退格删尾、回车/换行定行（已有未取行时忽略新定行符）；
 *  并进入字符环形缓冲（供 idle 实时回显）。返回是否入环形缓冲。
 *  \r 与 \n 均视为定行：串口终端回车常只发 \r。
 * v0.37（BUG-075）：定行即入挂起队列并清空组装行（让位下一行）——不再有"行就绪等待窗口"，
 * 多行粘贴/heredoc 的每一行都入队，读方慢时最多保留 KB_LINE_Q 行、超出行丢弃并告警。 */
int kb_feed_char(char c) {
    if (c == '\b') {
        if (line_len > 0) line_len--;
    } else if (c == '\n' || c == '\r') {
        line_buf[line_len] = 0;
        if (q_cnt < KB_LINE_Q) {
            int w = (q_head + q_cnt) % KB_LINE_Q;   /* 队尾槽：定行即入队 */
            int i;
            for (i = 0; i <= line_len; i++) line_q[w][i] = line_buf[i];
            q_cnt = q_cnt + 1;
        } else {
            kb_q_overflow = 1;      /* 队列满：丢本行（保留最老行，不覆写队头防并发撕裂） */
        }
        line_len = 0;               /* 组装行让位：后续字符组装下一行（防两行合并） */
        kb_report_drops();          /* OBS-R1: 行结束即上报本行丢弃的非 ASCII 字节数 */
        kb_report_overlong();       /* F10: 行结束上报本行是否被 128B 截断（如超长应提示用 heredoc） */
        if (line_hook) line_hook();
    } else if ((unsigned char)c >= 0x80u) {
        kb_nonascii_dropped++;      /* OBS-R1: 非 ASCII 高位字节不入行缓冲，累计计数 */
    } else if (c >= 32) {      /* 可打印字符：行未就绪才入行缓冲（v0.30 防两行合并） */
        if (line_len < KB_LINE_MAX) line_buf[line_len++] = c;
        else kb_line_overlong = 1;  /* 行满：丢弃本字符、标记本行被截断（KB_LINE_MAX 上限） */
    }
    /* 字符环形缓冲，供 idle 实时回显（含退格/回车） */
    int next = (head + 1) % KB_BUF;
    if (next != tail) { buf[head] = c; head = next; return 1; }
    return 0;
}

/* 处理一个扫描码（IRQ1 或宿主测试注入）；命中可打印键则入队，返回是否入队 */
int kb_feed_scan(uint8_t sc) {
    if (sc & 0x80) {              /* 松开事件 */
        uint8_t k = sc & 0x7F;
        if (k == 0x2A || k == 0x36) shift_down = 0;
        return 0;
    }
    if (sc == 0x2A || sc == 0x36) { /* 按下 Shift */
        shift_down = 1;
        return 0;
    }
    char c = shift_down ? sc_shift[sc] : sc_ascii[sc];
    if (c) return kb_feed_char(c);
    return 0;
}

int kb_getchar(void) {
    if (head == tail) return -1;
    char c = buf[tail];
    tail = (tail + 1) % KB_BUF;
    return (int)c;
}

void kb_init(void) {
    irq_install_handler(1, kb_cb);
}
