/* mini-os/v2-c-kernel/tests/fuzz_parse.c
 * 阶段二「加固」：宿主侧模糊测试（v0.29 候选）。
 * 对纯逻辑解析模块注入随机字节 / 随机路径，验证"畸形输入被拒绝而不崩溃"：
 *   - fs_walk 路径解析（fs_lookup/create/mkdir/rmdir/delete/list）
 *   - elf_load_range（畸形 ELF 头扫描：只解析不写内存）
 *   - net_eth_type / net_parse_arp_reply（以太网/ARP）
 *   - ip_parse / udp_parse / icmp_parse（IP/UDP/ICMP 协议解析）
 *   - dhcp_parse_reply（BOOTP 应答解析）
 * 确定性 PRNG（xorshift32 固定种子，可复现）；建议 ASan 下运行以抓越界/
 * 未初始化读/双重释放。正常跑完即通过（exit 0）；ASan/段错误即失败。
 * 迭代次数可用环境变量 FUZZ_ITERS 覆盖（默认 6 万轮 × ~6 次调用 ≈ 36 万次）。
 */
#include "blockdev.h"
#include "fs.h"
#include "elf.h"
#include "netutil.h"
#include "ip.h"
#include "udp.h"
#include "icmp.h"
#include "dhcp.h"
#include "slip.h"
#include "tcp_proto.h"      /* v1.1 Step 4 case 7：会话协议头解析 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- 确定性 PRNG（xorshift32） ---- */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng32(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}
static uint32_t rng_range(uint32_t n) { return n ? rng32() % n : 0; }
static uint8_t  rng_byte(void) { return (uint8_t)rng32(); }

/* 随机路径：0..cap-1 个字符（含 '/'、'.'、'..' 组件、空白、超长），NUL 结尾 */
static const char path_chars[] = "abcdefghijklmnopqrstuvwxyz0123456789/._- \t";
static void rand_path(char *p, uint32_t cap) {
    uint32_t n = rng_range(cap);
    for (uint32_t i = 0; i < n; i++)
        p[i] = path_chars[rng_range(sizeof(path_chars) - 1)];
    p[n] = '\0';
}

/* 随机字节缓冲（0..max），返回长度 */
static uint8_t *rand_buf(uint32_t *out_n, uint32_t max) {
    uint32_t n = rng_range(max + 1);
    uint8_t *b = (uint8_t *)malloc(n ? n : 1);
    if (!b) { fprintf(stderr, "fuzz: OOM\n"); exit(2); }
    for (uint32_t i = 0; i < n; i++) b[i] = rng_byte();
    *out_n = n;
    return b;
}

int main(void) {
    long iters = 60000;
    const char *e = getenv("FUZZ_ITERS");
    if (e && atol(e) > 0) iters = atol(e);

    /* ---- FS 内存盘（定期重置，避免 inode/块耗尽使 create 恒失败） ---- */
    uint32_t fs_blocks = 256;
    uint8_t *fsmem = (uint8_t *)malloc(fs_blocks * BLOCK_SIZE);
    blockdev_t bd;
    blockdev_init(&bd, fsmem, fs_blocks);
    fs_init(&bd);
    uint32_t since_reset = 0;

    for (long it = 0; it < iters; it++) {
        if (++since_reset >= 4096) { fs_init(&bd); since_reset = 0; }

        /* 1) 路径解析（fs_walk 核心）：lookup 常跑，写路径低频混合 */
        char path[64];
        rand_path(path, sizeof(path));
        fs_lookup(&bd, path);
        switch (rng_range(6)) {
        case 0: fs_create(&bd, path); break;
        case 1: fs_mkdir(&bd, path); break;
        case 2: fs_delete(&bd, path); break;
        case 3: fs_rmdir(&bd, path); break;
        case 4: { fs_dir_entry_t e[8]; (void)fs_list(&bd, path, e, 8); break; }
        default: break;                       /* 只 lookup */
        }

        /* 2) ELF 头扫描（只解析不写内存，畸形头/段表越界读由 ASan 抓） */
        {
            uint32_t n; uint8_t *b = rand_buf(&n, 512);
            uint32_t base = 0, end = 0;
            (void)elf_load_range(b, n, &base, &end);
            free(b);
        }

        /* 3) 以太网 / ARP */
        {
            uint32_t n; uint8_t *b = rand_buf(&n, 128);
            uint16_t et = 0; uint32_t sip = 0; uint8_t smac[6];
            (void)net_eth_type(b, n, &et);
            (void)net_parse_arp_reply(b, n, &sip, smac);
            free(b);
        }

        /* 4) IP / UDP / ICMP 协议解析（载荷指针指向 frame 内，长度越界由 ASan 抓） */
        {
            uint32_t n; uint8_t *b = rand_buf(&n, 300);
            uint32_t sip = 0, plen = 0; uint8_t proto = 0; const uint8_t *pay = 0;
            uint16_t sp = 0, dp = 0;
            uint8_t t = 0, c = 0; uint16_t id = 0, seq = 0;
            (void)ip_parse(b, n, &sip, &proto, &pay, &plen);
            (void)udp_parse(b, n, &sip, &sp, &dp, &pay, &plen);
            (void)icmp_parse(b, n, &sip, &t, &c, &id, &seq, &pay, &plen);
            free(b);
        }

        /* 5) DHCP 应答解析（BOOTP 头 + 选项，畸形选项长度越界由 ASan 抓） */
        {
            uint32_t n; uint8_t *b = rand_buf(&n, 512);
            uint8_t mt = 0; uint32_t yi = 0, si = 0, rt = 0, ls = 0;
            (void)dhcp_parse_reply(b, n, rng32(), &mt, &yi, &si, &rt, &ls);
            free(b);
        }

        /* 6) SLIP 增量解码（v1.1 Step 2：任意字节流 + 随机分段喂入，转义/END/溢出路径） */
        {
            slip_rx_t r; slip_rx_init(&r);
            uint32_t n; uint8_t *b = rand_buf(&n, 4096);
            for (uint32_t base = 0; base < n;) {
                uint32_t chunk = 1 + rng_range(8);           /* 随机一次喂入 1..8 字节 */
                if (chunk > n - base) chunk = n - base;
                for (uint32_t i = 0; i < chunk; i++) {
                    int st = slip_rx_feed(&r, b[base + i]);
                    if (st == 1 || st < 0) slip_rx_reset(&r); /* 帧就绪/协议错误：复位继收 */
                }
                base += chunk;
            }
            free(b);
        }

        /* 7) 虚拟 TCP 会话协议头解析（v1.1 Step 4：复制 PR13 模式，头不足/版本/保留位/越界由 ASan 抓） */
        {
            uint32_t n; uint8_t *b = rand_buf(&n, 64);
            uint32_t sid; uint8_t mt;
            (void)tcp_parse_hdr(b, n, &sid, &mt);
            free(b);
        }
    }

    printf("[fuzz_parse] %ld rounds (%ld parse calls) done, no crash (fixed seed, ASan clean)\n",
           iters, iters * 8);
    free(fsmem);
    return 0;
}
