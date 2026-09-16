/* golden 语料：M9g 转义池字节断言（\" 与 \x 贪心截断入产物哈希） */
int main() { char *s; char *t; s="a\"b"; t="x\x0A4z"; syscall3(1, s, 3, 0); syscall3(1, t, 3, 0); return 0; }
