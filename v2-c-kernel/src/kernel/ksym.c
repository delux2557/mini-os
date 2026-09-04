/* mini-os/v2-c-kernel/src/kernel/ksym.c
 * 加固 A-1 ③：panic 现场——寄存器 dump + EBP 调用栈回溯 + 地址到函数名解析。
 *
 * 依赖（均由 make 两阶段链接生成）：
 *   - ksym_tab.c：编译期由 tools/gen_kernsym.sh 抽 kernel_nosym.elf 的函数符号
 *     生成按地址升序的表。与用户态 qemu 的 addr2line 等价，但运行在裸机内，
 *     panic/异常时无需 host 介入即可把 eip/返回地址翻译成函数名。
 *
 * 关键前提：CFLAGS 已加 -fno-omit-frame-pointer，故内核函数都保帧指针（EBP 链），
 * 回溯合法；加 -fstack-protector-strong（加固 A-1 ①）与之一同生效。
 *
 * 设计取舍：paging 对低 16MB 恒等映射，故回溯读 EBP 链内存不会再次缺页；
 * 若某帧 EBP 被写坏指向未映射高地址，回溯会二次缺页——但该场景下内核本就
 * 处于"无法安全继续"的停机态，最坏结果仍是停机（无数据丢失，已打印先导），
 * 且 EBP 会先经范围/对齐过滤，把误入高地址的概率压到极低。
 */
#include "idt.h"
#include "serial.h"
#include "vga.h"
#include <stddef.h>
#include <stdint.h>

/* 由 gen_kernsym.sh 生成的已排序符号表（文本段函数） */
struct ksym { uint32_t addr; const char *name; };
extern const struct ksym ksym_tab[];
extern const unsigned int ksym_count;

/* 返回覆盖 addr 的最近函数符号名（二分）；无则 NULL。
 * 因表为"每个函数起始地址"升序，>addr 的最小起点前一符号即所属函数。 */
const char *ksym_name(uint32_t addr) {
    int lo = 0, hi = (int)ksym_count - 1, best = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if ((uint32_t)ksym_tab[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return NULL;
    return ksym_tab[best].name;
}

static const char *ksym_or(const char *s) {
    return s ? s : "?";
}

/* 沿 EBP 链回溯打印调用栈。ebp 为当前帧指针，eip 为当前指令地址。
 * 帧布局（i386，-fno-omit-frame-pointer）：
 *   [ebp]   = 上一帧 ebp
 *   [ebp+4] = 返回地址
 * 逐帧打印返回地址及其符号，最多 KBT_DEPTH 层以防御坏链。 */
#define KBT_DEPTH 24
/* 过滤 EBP：须 4 对齐且在低 16MB 恒等映射区域内（见文件头注释） */
#define KBT_EBP_OK(e) (((e) & 3u) == 0u && (e) >= 0x100u && (e) < 0x01000000u)

void dump_backtrace(uint32_t ebp, uint32_t eip) {
    serial_printf("[ksym] fault eip=%x  %s\n", eip, ksym_or(ksym_name(eip)));
    int i = 0;
    for (; i < KBT_DEPTH && KBT_EBP_OK(ebp); i++) {
        uint32_t *fr = (uint32_t *)ebp;
        uint32_t ra = fr[1];
        ebp = fr[0];
        serial_printf("[ksym]   #%d ret=%x  %s\n", i, ra, ksym_or(ksym_name(ra)));
        if (ra == 0) break;
    }
    if (i >= KBT_DEPTH) serial_puts("[ksym]   ...(depth cap)\n");
}

/* CPU 异常 / panic 统一现场打印：寄存器 + 该死地址符号 + （内核态）调用栈回溯。 */
void panic_dump(registers_t *r) {
    serial_printf("\n============= PANIC dump =============\n");
    serial_printf("int_no=%u err=%u  is_ring3=%u\n",
                  r->int_no, r->err_code, (r->cs & 3u) == 3u);
    serial_printf("eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
                  r->eax, r->ebx, r->ecx, r->edx);
    serial_printf("esi=%08x edi=%08x ebp=%08x esp=%08x\n",
                  r->esi, r->edi, r->ebp, r->esp);
    serial_printf("eip=%08x ( %s )  cs=%04x eflags=%08x\n",
                  r->eip, ksym_or(ksym_name(r->eip)), r->cs, r->eflags);
    /* 仅内核态故障可回溯（ring3 EBP 指向用户栈，不属内核文本）；且用 eflags.IF 提示 */
    if ((r->cs & 3u) != 3u) {
        serial_puts("[ksym] kernel-mode backtrace:\n");
        /* r->ebp 是 isr_common_stub pusha 压入的被打断函数的 EBP（见 idt.h registers_t） */
        dump_backtrace(r->ebp, r->eip);
    } else {
        serial_puts("[ksym] (ring3 fault, no kernel backtrace)\n");
    }
    serial_puts("========================================\n");
}