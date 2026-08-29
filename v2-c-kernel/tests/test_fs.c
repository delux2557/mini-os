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

    /* 8) inode 耗尽：用完剩余 inode 后创建失败（x.txt/y.txt 已占 2 个，
     *    加上根目录共 3 个已占用，故最多再建 FS_MAX_INODES-3 个） */
    int created = 0;
    while (created < 200) {
        char nm[32];
        sprintf(nm, "f%d.txt", created);
        if (fs_create(&bd, nm) < 0) break;
        created++;
    }
    CHECK(created >= FS_MAX_INODES - 3);                 /* 至少用满 61 个 */
    CHECK(created < 200);
    CHECK_EQ(fs_create(&bd, "one-more.txt"), -1);

    /* 9) blockdev 边界 */
    CHECK(blockdev_ptr(&bd, 0, 0) != 0);
    CHECK_EQ(blockdev_ptr(&bd, 256, 0), (void *)0);      /* 越界块 */
    CHECK_EQ(blockdev_ptr(&bd, 0, BLOCK_SIZE), (void *)0);/* 越界偏移 */

    free(mem);
    UTEST_SUMMARY("test_fs");
}
