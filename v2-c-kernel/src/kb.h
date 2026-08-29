/* mini-os/v2-c-kernel/kb.h
 * PS/2 键盘驱动（scan code set 1）。
 *  - 原始字符环形缓冲：kb_getchar() 轮询
 *  - v0.9 行缓冲：kb_feed_scan 同时组装行（退格/回车），供阻塞式 sys_readline 使用
 *  - 行完成回调：供内核在回车时唤醒阻塞的 readline 进程（纯逻辑，无内核依赖）
 */
#ifndef _KB_H
#define _KB_H
#include <stdint.h>

#define KB_LINE_MAX 128   /* 行缓冲容量（不含结尾 '\0'） */

typedef void (*kb_line_hook_t)(void);

void kb_init(void);
/* 取一个按键字符；无输入返回 -1 */
int kb_getchar(void);
/* 处理一个扫描码（供 IRQ1 调用，也可由宿主测试直接注入） */
int kb_feed_scan(uint8_t sc);
/* 注入一个已解析的 ASCII 字符（串口接收等非键盘输入源共用同一行缓冲） */
int kb_feed_char(char c);

/* ---- v0.9 行缓冲 ---- */
void kb_set_line_hook(kb_line_hook_t fn);  /* 一行输入完成时回调（可为 0） */
void kb_line_reset(void);                  /* 丢弃未完成行（清空行缓冲状态） */
int  kb_line_ready(void);                  /* 是否有完整行待取 */
/* 取走下一行到 out（最多 max-1 字节，NUL 结尾），返回长度；无行返回 -1 */
int  kb_line_take(char *out, uint32_t max);

#endif
