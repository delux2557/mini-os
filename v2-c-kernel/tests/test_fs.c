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

    /* 5) 写超上限：截断到最大文件大小；越界写返回 -1 */
    int i2 = fs_create(&bd, "big.txt");
    CHECK(i2 >= 0);
    uint32_t cap = FS_DIRECT_BLOCKS * BLOCK_SIZE;
    CHECK_EQ(fs_write(&bd, (uint32_t)i2, buf, cap - 10, 100), 10);   /* 只写进去 10 字节 */
    CHECK_EQ(fs_size(&bd, (uint32_t)i2), cap);
    CHECK_EQ(fs_write(&bd, (uint32_t)i2, buf, cap, 100), -1);        /* 越界 */
    CHECK_EQ(fs_delete(&bd, "big.txt"), 0);
    CHECK_EQ(fs_lookup(&bd, "big.txt"), -1);

    /* 6) 多文件 + ls 列出 */
    int i3 = fs_create(&bd, "x.txt");
    int i4 = fs_create(&bd, "y.txt");
    CHECK(i3 >= 0 && i4 >= 0);
    int n = fs_list(&bd, ents, FS_MAX_INODES);
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
