/* mini-os/v2-c-kernel/kb.c
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

/* ---- v0.9 行缓冲：把按键组装成"一行"，供阻塞式 sys_readline 使用 ----
 * 退格删行尾、回车定行；行完成时调用回调（供内核唤醒阻塞的 readline 进程）。
 * 行缓冲与字符环形缓冲独立：字符缓冲仍供 idle 实时回显。 */
static char line_buf[KB_LINE_MAX + 1];
static int  line_len = 0;
static int  line_ready = 0;
static kb_line_hook_t line_hook = 0;

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

static void kb_cb(registers_t *r) {
    (void)r;
    kb_feed_scan(inb(0x60));
}

void kb_set_line_hook(kb_line_hook_t fn) { line_hook = fn; }

void kb_line_reset(void) { line_len = 0; line_ready = 0; kb_line_overlong = 0; }

int kb_line_ready(void) { return line_ready; }

int kb_line_take(char *out, uint32_t max) {
    if (!line_ready) return -1;
    uint32_t n = (uint32_t)line_len;
    if (n >= max) n = max - 1;
    uint32_t i;
    for (i = 0; i < n; i++) out[i] = line_buf[i];
    out[n] = 0;
    line_ready = 0;
    line_len = 0;
    kb_report_drops();   /* OBS-R1: 取行后同样兜底上报（换行上报已重置，此处恒零或补报） */
    kb_report_overlong(); /* F10: 取行后兜底上报截断提示（同 OBS-R1，已重置则恒零） */
    return (int)n;
}

/* 处理一个已解析的 ASCII 字符（键盘查表结果或串口注入）：
 *  行缓冲组装：退格删尾、回车/换行定行（已有未取行时忽略新定行符）；
 *  并进入字符环形缓冲（供 idle 实时回显）。返回是否入环形缓冲。
 *  \r 与 \n 均视为定行：串口终端回车常只发 \r。 */
int kb_feed_char(char c) {
    if (c == '\b') {
        if (line_len > 0) line_len--;
    } else if (c == '\n' || c == '\r') {
        if (!line_ready) {          /* 满行也可定行（len<=KB_LINE_MAX 恒成立） */
            line_buf[line_len] = 0;
            line_ready = 1;
            kb_report_drops();      /* OBS-R1: 行结束即上报本行丢弃的非 ASCII 字节数 */
            kb_report_overlong();   /* F10: 行结束上报本行是否被 128B 截断（如超长应提示用 heredoc） */
            if (line_hook) line_hook();
        }
    } else if ((unsigned char)c >= 0x80u) {
        kb_nonascii_dropped++;      /* OBS-R1: 非 ASCII 高位字节不入行缓冲，累计计数 */
    } else if (c >= 32) {      /* 可打印字符：行未就绪才入行缓冲（v0.30 防两行合并） */
        if (!line_ready) {
            if (line_len < KB_LINE_MAX) line_buf[line_len++] = c;
            else kb_line_overlong = 1;   /* 行满：丢弃本字符、标记本行被截断（KB_LINE_MAX 上限） */
        }
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
