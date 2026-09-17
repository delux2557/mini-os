/* golden 语料（M14goto 收口，host 侧 goto/label）：具名标签与 goto 的**求值**例。
 * - 前向：`goto e;` 跳过 `i=99;` ⇒ i 保持 1（值探针，纯编译断言看不出"跳错位置"）；
 * - 后向：`L:` 起计数到 4 退出 —— 后向 jmp 的 rel32 off-by 只有实跑能抓（本轮两处 off-by 即如此暴露）；
 * - 出环：`goto out;` 绕过 `r=r+100;` ⇒ r 保持 0；
 * - 值：i(1) + n(4) + r(0) = 5 ⇒ exit 列 5（非 0，避免与"恰好没跑对"混淆）。
 * - 只用 int 标量，不碰 cc500 无类型面的宽度分歧（见 README）⇒ cc500 / minicc / gcc 三方同值 5。 */
int main()
{
  int i;
  int n;
  int r;
  i = 1;
  goto e;
  i = 99;
  e:
  n = 0;
  L:
  n = n + 1;
  if (n < 4) goto L;
  r = 0;
  goto out;
  r = r + 100;
  out:
  return i + n + r;
}
