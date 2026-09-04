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
