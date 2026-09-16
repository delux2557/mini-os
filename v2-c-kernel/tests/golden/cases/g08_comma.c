/* golden 语料：M9f 双编译器子句表（sha256+运行值入基线）；改钉协议见 docs/README。
 * 全局 `int ga,gb;` + 局部 `int a,b,c;` 混合子句表，四值互链取值；末式 a+b+c-7 = -3，
 * 故 exit 列 253（0xFD 截断）——非零值是**有意**的：顺带锁住子句表取值与 exit 截断语义。 */
int ga, gb;
int main(){int a,b,c;a=1;b=ga+1;gb=b+1;c=gb;return a+b+c-7;}
