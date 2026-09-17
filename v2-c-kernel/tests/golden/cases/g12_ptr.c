/* golden 语料（#165 收口）：指针访问两形态的**求值**例。
 * - deref（`*s` / `*(s+k)`）与下标（`s[k]`）的**读**、以及 deref 与下标的**写**，各一次；
 * - 旧码 cc500 无 unary `*` ⇒ 本文件在 cc500 侧整体编不过，故它同时是 cc500 deref 发射
 *   路径的首个 golden 观测；minicc 侧则首次把 `p[i]` 脱糖路径压进产物哈希。
 * - 值域刻意只取 char 宽（两编译器宽度口径一致的那一半，`int *` 的宽度有意分歧见 README），
 *   故 cc500 / minicc / gcc 三方同值：读 'a'+'b'+'c' = 294；写回后 66+66 = 132；
 *   294 - 132 - 125 = 37 ⇒ exit 列 37（非 0，避免与"恰好没跑对"混淆）。
 * - 写目标选局部 `char c` 取址，不写字符串字面量——避免把 .rodata 写入 UB 烘进基线。 */
int main()
{
  char *s;
  char c;
  char *p;
  int a;
  int b;
  s = "abc";
  a = *s + s[1] + *(s + 2);
  c = 0;
  p = &c;
  *p = 65;
  s = &c;
  s[0] = 66;
  b = *s + c;
  return a - b - 125;
}
