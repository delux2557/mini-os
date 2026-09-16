/* golden 语料：M9f 空语句体位（do 体后独立空语句 + 空复合块），双编译器同构 */
int main(){int i;i=0;do i=i+1;while(i<3);{;}return i-3;}
