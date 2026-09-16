/* golden 语料：M9g 转义池字节断言（\" 与 \x 贪心截断入产物哈希）；改钉协议见 docs/README。
 * 注：cc500 无 deref（#165），串内容无法在程序内直读断言，故只能压进产物哈希。 */
int main() { char *s; char *t; s="a\"b"; t="x\x0A4z"; syscall3(1, s, 3, 0); syscall3(1, t, 3, 0); return 0; }
