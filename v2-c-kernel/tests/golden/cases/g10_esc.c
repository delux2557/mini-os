/* golden 语料：M9g 转义池字节断言（\" 与 \x 贪心截断入产物哈希）；改钉协议见 docs/README。
 * 注：本文件写于 cc500 无 deref（#165）时，串内容只能压进产物哈希；#165 收口后已有
 * g12_ptr.c 走值通道直读，本文件保留原形态（字节面）不动——哈希基线不受注释改动影响。 */
int main() { char *s; char *t; s="a\"b"; t="x\x0A4z"; syscall3(1, s, 3, 0); syscall3(1, t, 3, 0); return 0; }
