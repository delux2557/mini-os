/* mini-os/v2-c-kernel/tests/test_fs.c
 * 文件系统宿主单元测试：只编译 src/blockdev.c + src/fs.c（纯逻辑），
 * 用 malloc 的内存模拟块设备，验证格式化/创建/查找/读写/跨块/删除/上限/目录列表。
 */
#include "utest.h"
#include "blockdev.h"
#include "fs.h"
#include <stdlib.h>
#include <string.h>

int main(void) {
    uint8_t *mem = (uint8_t *)malloc(256 * BLOCK_SIZE);
    if (!mem) { fprintf(stderr, "no mem\n"); return 1; }
    blockdev_t bd;
    blockdev_init(&bd, mem, 256);
    fs_dir_entry_t ents[FS_MAX_INODES];
    char buf[300];
    char big[600];

    /* 1) 格式化：超级块 + 根目录就绪 */
    CHECK_EQ(fs_init(&bd), 0);
    uint32_t *super = (uint32_t *)(mem + 0);
    CHECK_EQ(super[0], FS_MAGIC);
    CHECK_EQ(super[1], 256u);
    CHECK_EQ(super[2], (uint32_t)FS_MAX_INODES);
    CHECK_EQ(fs_size(&bd, FS_ROOT_INODE), 0);

    /* 2) 创建/查找/非法名/重名 */
    int i1 = fs_create(&bd, "a.txt");
    CHECK(i1 >= 0);
    CHECK_EQ(fs_lookup(&bd, "a.txt"), i1);
    CHECK_EQ(fs_lookup(&bd, "nope.txt"), -1);
    CHECK_EQ(fs_create(&bd, "a.txt"), -1);                 /* 重名 */
    CHECK_EQ(fs_create(&bd, ""), -1);                      /* 空名 */
    CHECK_EQ(fs_create(&bd, "this-name-is-way-too-long-for-the-fs"), -1);

    /* 3) 单块写入/读出 */
    CHECK_EQ(fs_write(&bd, (uint32_t)i1, "hello fs", 0, 8), 8);
    CHECK_EQ(fs_size(&bd, (uint32_t)i1), 8);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(fs_read(&bd, (uint32_t)i1, buf, 0, 8), 8);
    CHECK(strcmp(buf, "hello fs") == 0);
    CHECK_EQ(fs_read(&bd, (uint32_t)i1, buf, 8, 100), 0);  /* 越界读返回 0 */

    /* 3a) BUG-057 两级权限：系统文件只读不可删/不可写；用户文件可写可删 */
    {
        int s = fs_create(&bd, "/sys.txt");
        CHECK(s >= 0);
        CHECK_EQ(fs_protect(&bd, "/sys.txt"), 0);
        CHECK_EQ(fs_is_ro(&bd, (uint32_t)s), 1);
        CHECK_EQ(fs_write(&bd, (uint32_t)s, "x", 0, 1), -1);   /* 只读不可写 */
        CHECK_EQ(fs_delete(&bd, "/sys.txt"), -1);              /* 只读不可删 */
        CHECK_EQ(fs_lookup(&bd, "/sys.txt"), s);               /* 仍在 */
        CHECK_EQ(fs_protect(&bd, "/sys.txt"), 0);              /* protect 幂等 */
        /* 用户文件不受影响 */
        int u = fs_create(&bd, "/user.txt");
        CHECK(u >= 0);
        CHECK_EQ(fs_is_ro(&bd, (uint32_t)u), 0);
        CHECK_EQ(fs_write(&bd, (uint32_t)u, "y", 0, 1), 1);
        CHECK_EQ(fs_delete(&bd, "/user.txt"), 0);
        CHECK_EQ(fs_is_ro(&bd, 9999u), -1);                    /* 非法 inode */
        /* 注：sys.txt 因只读无法删除，保留占用 1 个 inode，见 8) 阈值调整。 */
    }

    /* 4) 跨块写入/读出（3 块 = 12KB），随机偏移抽查 + 跨块界覆写 */
    {
        char chunk[100];
        memset(chunk, 'X', sizeof(chunk));
        uint32_t written = 0;
        while (written < 10000) {
            uint32_t n = 10000 - written; if (n > 100) n = 100;
            CHECK_EQ(fs_write(&bd, (uint32_t)i1, chunk, written, n), (int)n);
            written += n;
        }
        CHECK_EQ(fs_size(&bd, (uint32_t)i1), 10000);
        for (uint32_t off = 0; off < 10000; off += 997) {
            uint32_t n = 10000 - off; if (n > 50) n = 50;
            memset(buf, 0, sizeof(buf));
            CHECK_EQ(fs_read(&bd, (uint32_t)i1, buf, off, n), (int)n);
            for (uint32_t k = 0; k < n; k++) CHECK_EQ((unsigned char)buf[k], (unsigned char)'X');
        }
        /* 跨块界覆写：写 500 字节（分 5 次 × 100） */
        memset(chunk, 'Y', sizeof(chunk));
        written = 0;
        while (written < 500) {
            uint32_t n = 500 - written; if (n > 100) n = 100;
            CHECK_EQ(fs_write(&bd, (uint32_t)i1, chunk, 4096 - 100 + written, n), (int)n);
            written += n;
        }
        memset(big, 0, sizeof(big));
        CHECK_EQ(fs_read(&bd, (uint32_t)i1, big, 4096 - 100, 500), 500);
        for (uint32_t k = 0; k < 500; k++) CHECK_EQ((unsigned char)big[k], (unsigned char)'Y');
    }

    /* 5) v0.14 跨入间接块 + 超上限边界 */
    int i2 = fs_create(&bd, "big.txt");
    CHECK(i2 >= 0);
    {
        uint32_t cap = FS_DIRECT_BLOCKS * BLOCK_SIZE;
        char z[100];
        memset(z, 'Z', sizeof(z));
        CHECK_EQ(fs_write(&bd, (uint32_t)i2, z, cap - 10, 100), 100);  /* 跨入间接块 */
        CHECK_EQ(fs_size(&bd, (uint32_t)i2), cap - 10 + 100);
        memset(big, 0, sizeof(big));
        CHECK_EQ(fs_read(&bd, (uint32_t)i2, big, cap - 10, 100), 100); /* 跨块界读回 */
        for (uint32_t k = 0; k < 100; k++)
            CHECK_EQ((unsigned char)big[k], (unsigned char)'Z');
        CHECK_EQ(fs_write(&bd, (uint32_t)i2, buf, FS_MAX_FILE_SIZE, 1), -1);        /* 越界 */
        /* 文件块号 1035（间接块末）：len 被截断到上限，恰写 1 字节 */
        CHECK_EQ(fs_write(&bd, (uint32_t)i2, buf, FS_MAX_FILE_SIZE - 1, 2), 1);
        CHECK_EQ(fs_size(&bd, (uint32_t)i2), FS_MAX_FILE_SIZE);
    }
    CHECK_EQ(fs_delete(&bd, "big.txt"), 0);
    CHECK_EQ(fs_lookup(&bd, "big.txt"), -1);

    /* 6) 多文件 + ls 列出 */
    int i3 = fs_create(&bd, "x.txt");
    int i4 = fs_create(&bd, "y.txt");
    CHECK(i3 >= 0 && i4 >= 0);
    int n = fs_list(&bd, "/", ents, FS_MAX_INODES);
    CHECK(n >= 3);
    int seen_a = 0, seen_x = 0, seen_y = 0;
    for (int k = 0; k < n; k++) {
        if (strcmp(ents[k].name, "a.txt") == 0) seen_a = 1;
        if (strcmp(ents[k].name, "x.txt") == 0) seen_x = 1;
        if (strcmp(ents[k].name, "y.txt") == 0) seen_y = 1;
    }
    CHECK(seen_a && seen_x && seen_y);

    /* 7) 删除后不可见、inode/数据块被释放可复用 */
    CHECK_EQ(fs_delete(&bd, "a.txt"), 0);
    CHECK_EQ(fs_lookup(&bd, "a.txt"), -1);
    CHECK_EQ(fs_delete(&bd, "a.txt"), -1);               /* 再删报错 */

    /* ---- v0.14 目录层级 ---- */
    {
        /* 建 /d1/d2/f.txt，写入后列出验证类型 */
        int d1 = fs_mkdir(&bd, "/d1");
        int d2 = fs_mkdir(&bd, "/d1/d2");
        CHECK(d1 >= 0 && d2 >= 0);
        CHECK_EQ(fs_mkdir(&bd, "/d1"), -1);              /* 重名 */
        CHECK_EQ(fs_mkdir(&bd, "/none/x"), -1);          /* 父目录不存在 */
        int f = fs_create(&bd, "/d1/../d1/d2/f.txt");    /* ".." 解析后仍指向 /d1/d2 */
        CHECK(f >= 0);
        CHECK_EQ(fs_write(&bd, (uint32_t)f, "hi", 0, 2), 2);
        CHECK_EQ(fs_size(&bd, (uint32_t)f), 2);
        CHECK(fs_lookup(&bd, "/d1/d2/f.txt") >= 0);
        CHECK_EQ(fs_lookup(&bd, "/d1"), d1);
        CHECK_EQ(fs_lookup(&bd, "/d1/d2"), d2);
        CHECK_EQ(fs_lookup_in(&bd, (uint32_t)d2, "f.txt"), f);
        CHECK_EQ(fs_lookup(&bd, "/d1/nope"), -1);

        /* 非空/类型错误 */
        CHECK_EQ(fs_rmdir(&bd, "/d1/d2"), -1);           /* 非空目录 */
        CHECK_EQ(fs_delete(&bd, "/d1/d2"), -1);          /* 目录不可 delete */
        CHECK_EQ(fs_rmdir(&bd, "/d1/d2/f.txt"), -1);     /* rmdir 非目录 */
        CHECK_EQ(fs_mkdir(&bd, "/d1/d2/f.txt/x"), -1);   /* 中间组件是文件 */

        /* list：类型标记（目录带 DIR） */
        fs_dir_entry_t sub[8];
        int sn = fs_list(&bd, "/d1/d2", sub, 8);
        CHECK_EQ(sn, 1);
        CHECK(strcmp(sub[0].name, "f.txt") == 0);
        CHECK_EQ(sub[0].type, FS_TYPE_FILE);
        int sn2 = fs_list(&bd, "/d1", sub, 8);
        CHECK_EQ(sn2, 1);
        CHECK(strcmp(sub[0].name, "d2") == 0);
        CHECK_EQ(sub[0].type, FS_TYPE_DIR);
        CHECK_EQ(fs_list(&bd, "/d1/d2/f.txt", sub, 8), -1); /* 列出文件 -> 错误 */

        /* 清理 */
        CHECK_EQ(fs_delete(&bd, "/d1/d2/f.txt"), 0);
        CHECK_EQ(fs_rmdir(&bd, "/d1/d2"), 0);
        CHECK_EQ(fs_rmdir(&bd, "/d1"), 0);
        CHECK_EQ(fs_lookup(&bd, "/d1"), -1);

        /* ".." 与重复斜杠、根目录 ".." 仍为根 */
        CHECK(fs_mkdir(&bd, "/a") >= 0);
        CHECK(fs_mkdir(&bd, "/a/b") >= 0);
        CHECK(fs_mkdir(&bd, "/a/b/../c") >= 0);          /* 在 /a 下建 c */
        CHECK(fs_lookup(&bd, "/a/c") >= 0);
        CHECK_EQ(fs_lookup(&bd, "/a/b/../c"), fs_lookup(&bd, "/a/c"));
        CHECK(fs_mkdir(&bd, "//dup") >= 0);              /* 重复斜杠建 /dup */
        CHECK(fs_mkdir(&bd, "//dup//x") >= 0);
        CHECK(fs_lookup(&bd, "/dup/x") >= 0);
        CHECK(fs_mkdir(&bd, "/../root2") >= 0);          /* 根目录 .. 仍是根 */
        CHECK(fs_lookup(&bd, "/root2") >= 0);
        CHECK_EQ(fs_rmdir(&bd, "/a/c"), 0);
        CHECK_EQ(fs_rmdir(&bd, "/a/b"), 0);
        CHECK_EQ(fs_rmdir(&bd, "/a"), 0);
        CHECK_EQ(fs_rmdir(&bd, "/dup/x"), 0);
        CHECK_EQ(fs_rmdir(&bd, "/dup"), 0);
        CHECK_EQ(fs_rmdir(&bd, "/root2"), 0);
    }

    /* ---- v0.14 间接块：100000 字节大文件（25 块 > 12 直接块） ---- */
    {
        int ib = fs_create(&bd, "/big.bin");
        CHECK(ib >= 0);
        uint8_t chunk[128];
        uint32_t total = 100000;
        uint32_t off = 0;
        while (off < total) {
            uint32_t n = total - off; if (n > 128) n = 128;
            for (uint32_t k = 0; k < n; k++) chunk[k] = (uint8_t)((off + k) & 0xFF);
            CHECK_EQ(fs_write(&bd, (uint32_t)ib, chunk, off, n), (int)n);
            off += n;
        }
        CHECK_EQ(fs_size(&bd, (uint32_t)ib), total);
        /* 抽查：偏移 0 / 直接块末 / 间接块首 / 文件末 */
        uint32_t spots[4];
        spots[0] = 0; spots[1] = 12 * 4096 - 1; spots[2] = 12 * 4096; spots[3] = total - 1;
        for (int s = 0; s < 4; s++) {
            uint8_t c = 0xAA;
            CHECK_EQ(fs_read(&bd, (uint32_t)ib, &c, spots[s], 1), 1);
            CHECK_EQ((unsigned)c, (unsigned)(uint8_t)(spots[s] & 0xFF));
        }
        /* 全量一致性：997 步长抽样读回 */
        for (uint32_t off2 = 0; off2 < total; off2 += 997) {
            uint32_t n = total - off2; if (n > 64) n = 64;
            memset(big, 0, sizeof(big));
            CHECK_EQ(fs_read(&bd, (uint32_t)ib, big, off2, n), (int)n);
            for (uint32_t k = 0; k < n; k++)
                CHECK_EQ((unsigned char)big[k], (unsigned char)(uint8_t)((off2 + k) & 0xFF));
        }
        CHECK_EQ(fs_delete(&bd, "/big.bin"), 0);
        CHECK_EQ(fs_lookup(&bd, "/big.bin"), -1);
    }

    /* ---- 独立安全评估修复回归（SEC-03/04/05，2026-09-04）----
     * 置于 8) inode 耗尽之前：三组用例均自行回收占用的 inode/块，
     * 不扰动其"仍可建满 FS_MAX_INODES-4 个"的预算断言。 */

    /* SEC-03：free_inode_blocks 遇越界块号（恶意镜像篡改 inode）不得越界写数据块位图。
     * 用原始内存偏移直改 inode 槽（INODE_TABLE_BLK=3，fs_inode_t 64B）模拟被篡改镜像；
     * 若位图被非法索引污染，其后新建必然异常。 */
    {
        int f = fs_create(&bd, "/sec03.txt");
        CHECK(f >= 0);
        CHECK_EQ(fs_write(&bd, (uint32_t)f, "abc", 0, 3), 3);
        fs_inode_t *in = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)f * sizeof(fs_inode_t));
        in->blocks[1]  = 0xFFFFFFF0u;      /* 越界直接块号：释放时应跳过 */
        in->indirect   = bd.blocks + 5;    /* 越界间接块号：释放时应跳过 */
        CHECK_EQ(fs_delete(&bd, "/sec03.txt"), 0);
        int g = fs_create(&bd, "/sec03b.txt");   /* 位图未被污染则分配正常 */
        CHECK(g >= 0);
        CHECK_EQ(fs_delete(&bd, "/sec03b.txt"), 0);
    }

    /* SEC-04：超长分量（>FS_MAX_NAME-1=23 字符）不得被静默截断续解析。
     * 先用 23 字符目录 d23 与 d23/ZZ 作"诱饵"（旧码会把超长首分量截成 d23 后级联吃进 ZZ），
     * 再以 25 字符首分量路径建文件：新码应 -1 拒判，诱饵目录内不得被误建。 */
    {
        char d23[FS_MAX_NAME];
        memset(d23, 'd', FS_MAX_NAME - 1); d23[FS_MAX_NAME - 1] = 0;   /* 恰 23 字符 */
        int q = fs_mkdir(&bd, d23);
        CHECK(q >= 0);
        char sub[48];
        sprintf(sub, "/%s/ZZ", d23);
        CHECK(fs_mkdir(&bd, sub) >= 0);          /* 诱饵：23 字分量正常建目录 */
        char p[80];
        sprintf(p, "/%sZZ/x.txt", d23);          /* 首分量 = d23+"ZZ" = 25 字（超长） */
        CHECK_EQ(fs_create(&bd, p), -1);         /* 新码拒判 */
        CHECK(fs_lookup(&bd, p) < 0);
        CHECK(fs_lookup(&bd, "/d23/ZZ/x.txt") < 0);  /* 诱饵目录内不得被误建 */
        CHECK_EQ(fs_rmdir(&bd, sub), 0);
        CHECK_EQ(fs_rmdir(&bd, d23), 0);
    }

    /* SEC-05：目录级只读——fs_mkdir/fs_create 不得绕过父目录 FS_MODE_RO。
     * 测试后手动清 inode.mode 解除只读以回收目录（避免扰动下方 inode 预算）。 */
    {
        int r = fs_mkdir(&bd, "/rosec05");
        CHECK(r >= 0);
        CHECK_EQ(fs_protect(&bd, "/rosec05"), 0);
        CHECK_EQ(fs_is_ro(&bd, (uint32_t)r), 1);
        CHECK_EQ(fs_create(&bd, "/rosec05/f.txt"), -1);   /* 新码拒建 */
        CHECK_EQ(fs_mkdir(&bd, "/rosec05/sub"), -1);      /* 新码拒建目录 */
        CHECK(fs_lookup(&bd, "/rosec05/f.txt") < 0);
        fs_inode_t *roin = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)r * sizeof(fs_inode_t));
        roin->mode = 0;                                   /* 清只读位后回收 */
        CHECK_EQ(fs_rmdir(&bd, "/rosec05"), 0);
    }

    /* ---- RD3 红队第三轮：fs 读/写/目录遍历的块号信任（BUG-069 RD3-V2 / BUG-070 RD3-V1）----
     * 恶意镜像篡改 inode 的 blocks[]/indirect，若把"数据区块号"直接用于 blockdev 寻址且不校验：
     *  V2：块号指向 inode 表（如 3）→ 读/写别名作用于 inode 表，静默击穿 BUG-057 系统文件只读；
     *  V1：越界块号（0xFFFFFFF0）→ blockdev_ptr 返 NULL → 调用方解引用崩（ASan SEGV）。
     * 修复（blk_valid 推广到一切寻址路径）：损坏块号在读写返回 0/-1 被拒、目录遍历跳过损坏块。
     * 注：用原始内存偏移直改 inode 槽模拟被篡改镜像；全部用例自行回收 inode，扰动下方 8) 预算。 */
    {
        /* ---- V2：文件名块号指向 inode 表，不得别名读写 / 击穿 BUG-057 ---- */
        int va = fs_create(&bd, "/rd3v2_a");          /* 普通文件 A */
        CHECK(va >= 0);
        int vp = fs_create(&bd, "/rd3v2_p");          /* 受保护文件 P */
        CHECK(vp >= 0);
        CHECK_EQ(fs_write(&bd, (uint32_t)va, "OLD", 0, 3), 3);
        CHECK_EQ(fs_write(&bd, (uint32_t)vp, "PROT", 0, 4), 4);
        CHECK_EQ(fs_protect(&bd, "/rd3v2_p"), 0);
        CHECK_EQ(fs_is_ro(&bd, (uint32_t)vp), 1);
        /* 篡改 A 的 blocks[0]=3（inode 表块）：读/写须拒绝，不得落到 inode 表 */
        fs_inode_t *avin = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)va * sizeof(fs_inode_t));
        uint32_t v2sav = avin->blocks[0];
        avin->blocks[0] = 3u;                          /* 指向 inode 表 */
        memset(buf, 0, sizeof(buf));
        CHECK(fs_read(&bd, (uint32_t)va, buf, 0, 3) < 3);       /* 拒绝/截断，不读回 inode 表 */
        CHECK_EQ(fs_write(&bd, (uint32_t)va, "X", 0, 1), -1);   /* 首块损坏 → -1，不写 inode 表 */
        /* P 只读位与内容未被 A 的"写 inode 表"链路击穿 */
        CHECK_EQ(fs_is_ro(&bd, (uint32_t)vp), 1);
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(fs_read(&bd, (uint32_t)vp, buf, 0, 4), 4);
        CHECK(strcmp(buf, "PROT") == 0);
        CHECK_EQ(fs_write(&bd, (uint32_t)vp, "x", 0, 1), -1);   /* P 仍只读 */
        avin->blocks[0] = v2sav;                        /* 恢复后正常清理 */
        CHECK_EQ(fs_delete(&bd, "/rd3v2_a"), 0);
        CHECK_EQ(fs_delete(&bd, "/rd3v2_p"), -1);       /* P 只读不可删，清位回收 */
        ((fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)vp * sizeof(fs_inode_t)))->mode = 0;
        CHECK_EQ(fs_delete(&bd, "/rd3v2_p"), 0);
    }

    {
        /* ---- V1a：文件直接块=0xFFFFFFF0：读截断为 0、写返 -1，不崩 ---- */
        int f1 = fs_create(&bd, "/rd3v1_f");
        CHECK(f1 >= 0);
        CHECK_EQ(fs_write(&bd, (uint32_t)f1, "hello", 0, 5), 5);
        fs_inode_t *f1in = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)f1 * sizeof(fs_inode_t));
        uint32_t v1sav = f1in->blocks[0];
        f1in->blocks[0] = 0xFFFFFFF0u;
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(fs_read(&bd, (uint32_t)f1, buf, 0, 5), 0);     /* 损坏点截断 */
        CHECK_EQ(fs_write(&bd, (uint32_t)f1, "X", 0, 1), -1);   /* 首块损坏 → -1 */
        f1in->blocks[0] = v1sav;
        CHECK_EQ(fs_delete(&bd, "/rd3v1_f"), 0);

        /* ---- V1b：间接块指针=0xFFFFFFF0：间接区读截断为 0、写返 -1，不崩 ---- */
        int f2 = fs_create(&bd, "/rd3v1_i");
        CHECK(f2 >= 0);
        uint32_t cap2 = FS_DIRECT_BLOCKS * BLOCK_SIZE;
        CHECK_EQ(fs_write(&bd, (uint32_t)f2, "tail", (int)cap2, 4), 4);  /* 分配间接块区 */
        CHECK(fs_size(&bd, (uint32_t)f2) > cap2);
        fs_inode_t *f2in = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)f2 * sizeof(fs_inode_t));
        uint32_t v2sid = f2in->indirect;
        f2in->indirect = 0xFFFFFFF0u;
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(fs_read(&bd, (uint32_t)f2, buf, (int)cap2, 4), 0);    /* 间接区读截断 */
        CHECK_EQ(fs_write(&bd, (uint32_t)f2, "X", (int)cap2, 1), -1);  /* 首块(间接)损坏 → -1 */
        f2in->indirect = v2sid;
        CHECK_EQ(fs_delete(&bd, "/rd3v1_i"), 0);

        /* ---- V1c：目录直接块=0xFFFFFFF0：lookup/list 跳过损坏块、不崩 ---- */
        int d1 = fs_mkdir(&bd, "/rd3v1_d");
        CHECK(d1 >= 0);
        int de = fs_create(&bd, "/rd3v1_d/x.txt");
        CHECK(de >= 0);
        fs_inode_t *din = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)d1 * sizeof(fs_inode_t));
        uint32_t dv[FS_DIRECT_BLOCKS];
        for (int k = 0; k < FS_DIRECT_BLOCKS; k++) dv[k] = din->blocks[k];
        for (int k = 0; k < FS_DIRECT_BLOCKS; k++) din->blocks[k] = 0xFFFFFFF0u; /* 全损 */
        fs_dir_entry_t sub[8];
        CHECK_EQ(fs_lookup_in(&bd, (uint32_t)d1, "x.txt"), -1);  /* 损坏块被跳过 → 找不到 */
        CHECK_EQ(fs_list_dir(&bd, (uint32_t)d1, sub, 8), 0);     /* 全损 → 空列表，不崩 */
        for (int k = 0; k < FS_DIRECT_BLOCKS; k++) din->blocks[k] = dv[k];  /* 恢复 */
        CHECK_EQ(fs_lookup_in(&bd, (uint32_t)d1, "x.txt"), de);  /* 正常对照：校验不误伤 */
        CHECK_EQ(fs_delete(&bd, "/rd3v1_d/x.txt"), 0);
        CHECK_EQ(fs_rmdir(&bd, "/rd3v1_d"), 0);
    }

    /* ---- RD5 红队四轮：块归属账本（BUG-071），挡"合法范围内重复块"跨文件读写/RO 绕过 ----
     * Tier-1 blk_valid 只校验块号∈[FS_DATA_START, blocks)，挡不住恶意镜像把两个 inode 的
     * blocks[]/indirect 指向"同一合法数据块"。账本把每数据块登记到唯一 owner inode：
     * 挂载外部镜像后 fs_scan_owners 重建并检测重复（violations>0）；访问"非本 inode 所有"的块
     * 在 file_block 被 owner_check 阻断（读截断 0 / 写返 -1），不走位图重复分配、不改他人块。
     * 镜像损坏态全部在"篡改→scan→读/写断言→恢复→删除"内闭环，块面/位图回到一致，不扰动 8) 预算。 */

    /* ---- 健康基线：干净镜像重建账本应 0 冲突、0 孤儿（无假阳性） ---- */
    {
        fs_scan_owners(&bd);
        CHECK_EQ(fs_owner_violations_get(), 0u);   /* 正常镜像无重复块 */
        CHECK_EQ(fs_owner_orphans(&bd), 0u);       /* 位图在用块均有主 */
    }

    /* ---- V4 跨文件重复：B 的 blocks[0] 指向 A 的合法数据块 ----
     * scan 检出冲突；经 B 读/写该块被 owner_check 阻断；A 自身读写不受影响。
     * 先建先得 ino，故 A 成为共享块 owner，B 是被阻断方（确定性）。 */
    {
        int ra = fs_create(&bd, "/v4_a");
        int rb = fs_create(&bd, "/v4_b");
        CHECK(ra >= 0 && rb >= 0 && ra < rb);
        CHECK_EQ(fs_write(&bd, (uint32_t)ra, "AAA", 0, 3), 3);
        CHECK_EQ(fs_write(&bd, (uint32_t)rb, "BBB", 0, 3), 3);
        fs_inode_t *abin = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)ra * sizeof(fs_inode_t));
        fs_inode_t *bbin = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)rb * sizeof(fs_inode_t));
        uint32_t ablk = abin->blocks[0];
        uint32_t bSav = bbin->blocks[0];
        CHECK(ablk >= FS_DATA_START && ablk < bd.blocks);   /* 合法范围内重复的基础 */
        bbin->blocks[0] = ablk;                 /* 恶意镜像：B 指向 A 的数据块 */
        fs_scan_owners(&bd);                    /* 模拟挂载该镜像重建账本 */
        CHECK(fs_owner_violations_get() > 0);   /* 检出"合法范围内重复块" */
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(fs_read(&bd, (uint32_t)rb, buf, 0, 3), 0);      /* 读被阻断（不读回 A 数据） */
        CHECK_EQ(fs_write(&bd, (uint32_t)rb, "XXX", 0, 3), -1);  /* 写被阻断（首块 owner 不符） */
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(fs_read(&bd, (uint32_t)ra, buf, 0, 3), 3);      /* A 自身不受影响 */
        CHECK(strcmp(buf, "AAA") == 0);
        bbin->blocks[0] = bSav;                 /* 恢复后正常清理 */
        CHECK_EQ(fs_delete(&bd, "/v4_b"), 0);
        CHECK_EQ(fs_delete(&bd, "/v4_a"), 0);
    }

    /* ---- V4 × RO 权限绕过：可写文件 W 的块指向受保护文件 P 的数据块 ----
     * BUG-057 只查"写者自身 mode"；账本在数据面按"块 owner"二次拦截——即使写者在别处
     * 可写，经它写 P 的块也首块即拒，RO 内容与只读位不被击穿。 */
    {
        int rp = fs_create(&bd, "/v4_p");
        int rw = fs_create(&bd, "/v4_w");
        CHECK(rp >= 0 && rw >= 0 && rp < rw);
        CHECK_EQ(fs_write(&bd, (uint32_t)rp, "PROT", 0, 4), 4);
        CHECK_EQ(fs_protect(&bd, "/v4_p"), 0);
        CHECK_EQ(fs_write(&bd, (uint32_t)rw, "WWW", 0, 3), 3);
        fs_inode_t *pin = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)rp * sizeof(fs_inode_t));
        fs_inode_t *win = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)rw * sizeof(fs_inode_t));
        uint32_t plbk = pin->blocks[0];
        uint32_t wSav = win->blocks[0];
        win->blocks[0] = plbk;                  /* W 指向受保护文件的数据块 */
        fs_scan_owners(&bd);
        CHECK(fs_owner_violations_get() > 0);
        CHECK_EQ(fs_write(&bd, (uint32_t)rw, "HACK", 0, 4), -1); /* 可写者也不能写 P 的块 */
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(fs_read(&bd, (uint32_t)rw, buf, 0, 3), 0);      /* 经 W 读 P 的块被阻断 */
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(fs_read(&bd, (uint32_t)rp, buf, 0, 4), 4);      /* P 内容未变 */
        CHECK(strcmp(buf, "PROT") == 0);
        CHECK_EQ(fs_is_ro(&bd, (uint32_t)rp), 1);               /* P 只读位未击穿 */
        win->blocks[0] = wSav;
        CHECK_EQ(fs_delete(&bd, "/v4_w"), 0);
        CHECK_EQ(fs_delete(&bd, "/v4_p"), -1);   /* P 只读不可删，清位后回收 */
        ((fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)rp * sizeof(fs_inode_t)))->mode = 0;
        CHECK_EQ(fs_delete(&bd, "/v4_p"), 0);
    }

    /* ---- V4 间接块重复：Y 的 indirect 指向大文件 X 的间接块（同一合法块）----
     * scan 经间接块路径检出冲突；经 Y 越直接区读/写被 owner_check 阻断
     * （间接块本身须归本 inode 所有）。 */
    {
        int rx = fs_create(&bd, "/v4_ind_x");
        int ry = fs_create(&bd, "/v4_ind_y");
        CHECK(rx >= 0 && ry >= 0 && rx < ry);
        uint32_t capx = FS_DIRECT_BLOCKS * BLOCK_SIZE;
        char zz[20]; memset(zz, 'Z', sizeof(zz));
        CHECK_EQ(fs_write(&bd, (uint32_t)rx, zz, (int)capx, 10), 10); /* X 获得间接块+间接数据 */
        CHECK_EQ(fs_write(&bd, (uint32_t)ry, "Y", 0, 1), 1);
        fs_inode_t *xin = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)rx * sizeof(fs_inode_t));
        fs_inode_t *yin = (fs_inode_t *)(mem + (3u * BLOCK_SIZE) + (uint32_t)ry * sizeof(fs_inode_t));
        uint32_t xInd = xin->indirect;
        uint32_t ySav = yin->indirect;          /* Y 正常无间接块，应为 0 */
        CHECK_EQ(ySav, 0u);
        yin->indirect = xInd;                   /* Y 复用 X 的间接块（合法范围内） */
        fs_scan_owners(&bd);
        CHECK(fs_owner_violations_get() > 0);
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(fs_read(&bd, (uint32_t)ry, buf, (int)capx, 4), 0);   /* 间接区读被阻断 */
        CHECK_EQ(fs_write(&bd, (uint32_t)ry, "X", (int)capx, 1), -1); /* 写被阻断（间接块 owner 不符） */
        yin->indirect = ySav;
        CHECK_EQ(fs_delete(&bd, "/v4_ind_y"), 0);
        CHECK_EQ(fs_delete(&bd, "/v4_ind_x"), 0);
    }

    /* 8) inode 耗尽：用完剩余 inode 后创建失败（x.txt/y.txt 已占 2 个 + 3a) 的只读
     *   /sys.txt 占 1 个，加上根目录共 4 个已占用，故最多再建 FS_MAX_INODES-4 个） */
    int created = 0;
    while (created < 200) {
        char nm[32];
        sprintf(nm, "f%d.txt", created);
        if (fs_create(&bd, nm) < 0) break;
        created++;
    }
    CHECK(created >= FS_MAX_INODES - 4);                 /* 至少用满剩余 */
    CHECK(created < 200);
    CHECK_EQ(fs_create(&bd, "one-more.txt"), -1);

    /* 9) blockdev 边界 */
    CHECK(blockdev_ptr(&bd, 0, 0) != 0);
    CHECK_EQ(blockdev_ptr(&bd, 256, 0), (void *)0);      /* 越界块 */
    CHECK_EQ(blockdev_ptr(&bd, 0, BLOCK_SIZE), (void *)0);/* 越界偏移 */

    free(mem);
    UTEST_SUMMARY("test_fs");
}
