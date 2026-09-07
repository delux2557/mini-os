/*
 * Copyright (C) 2006 Edmund GRIMLEY EVANS <edmundo@rano.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * A self-compiling compiler for a small subset of C.
 *
 * v0.27 mini-os 移植（自举工具链）：
 *  - code_offset 改为 0x800A0000（APP_LINK，与内核 ELF 加载器一致）
 *  - 唯一的机器码 stub = syscall3(n,a,b,c)（eax=n ebx=a ecx=b edx=c，int $0x80）
 *  - exit/malloc/getchar/putchar/sys_print 全部用 C 子集实现（经 syscall3）
 *  - 输入：整读 mini-fs 的 /cc500.c（initramfs 预置源码）进堆
 *  - 输出：编译完把 code 缓冲一次性写回 mini-fs 的 /out.elf
 *  - 自举验证：run cc500（gcc 版）编译自身 -> /out.elf = P1；再 run /out.elf
 *    （P1）编译自身 -> /out.elf = P2；shell 的 ccboot 校验 P1 与 P2 字节一致。
 */

/* Our library functions. */
void exit(int);
int getchar(void);
void *malloc(int);
int putchar(int);
int syscall3(int n, int a, int b, int c);   /* v0.27: be_start 内联 stub */
int sys_print(char *s);

/* The first thing defined must be the entry (v0.27: cc500_main；cc500_crt 也以它入口).
 * v0.27b: 支持命令行指定输入/输出路径——argv[1]=输入 argv[2]=输出。
 * 注意声明顺序：CC500 首个形参落在 8(%esp)、末个落在 4(%esp)，而内核/CRT 以
 * [esp+4]=argc、[esp+8]=argv 进入，故按 (char *argv, int argc) 声明才能对上。 */
int main1(char *argv, int argc);
int cc500_main(char *argv, int argc)
{
  return main1(argv, argc);
}

char *my_realloc(char *old, int oldlen, int newlen)
{
  char *new = malloc(newlen);
  int i = 0;
  while (i <= oldlen - 1) {
    new[i] = old[i];
    i = i + 1;
  }
  return new;
}

int nextc;
char *token;
int token_size;
int cc_depth;   /* OBS-CC-1：递归下降深度计数（编译期，不 emit，不影响 codegen） */

void error()
{
  /* v0.32 F-1：error() 带 token 上下文，学员可归因（此前裸 exit(1)，零诊断） */
  sys_print("cc500: error at\x0a");
  sys_print(token);
  sys_print("\x0a");
  exit(1);
}

int i;

void takechar()
{
  if (token_size <= i + 1) {
    int x = (i + 10) << 1;
    token = my_realloc(token, token_size, x);
    token_size = x;
  }
  token[i] = nextc;
  i = i + 1;
  nextc = getchar();
}

void get_token()
{
  int w = 1;
  int str_done;
  int c0;
  while (w) {
    w = 0;
    while ((nextc == ' ') | (nextc == 9) | (nextc == 10))
      nextc = getchar();
    i = 0;
    while ((('a' <= nextc) & (nextc <= 'z')) |
	   (('0' <= nextc) & (nextc <= '9')) | (nextc == '_'))
      takechar();
    /* M2：operator 词法精确化——原 while 把 <>=|&! 集合连续吞并，导致 "=!" 被合成
     * 单 token（x=!x 无法解析）。现只合成合法双字符运算符（== != <= >= << >> && ||），
     * 其余按单字符返回。对旧语法合法输入的 token 流不变（字节零变化由用例锁定）。 */
    if (i == 0) {
      c0 = nextc;
      if ((c0 == '<') | (c0 == '>') | (c0 == '=') | (c0 == '|') | (c0 == '&') | (c0 == '!')) {
	takechar();
	if (nextc == '=') {
	  if ((c0 == '<') | (c0 == '>') | (c0 == '=') | (c0 == '!'))
	    takechar();
	}
	else if (nextc == '<') {
	  if (c0 == '<')
	    takechar();
	}
	else if (nextc == '>') {
	  if (c0 == '>')
	    takechar();
	}
	else if (nextc == '&') {
	  if (c0 == '&')
	    takechar();
	}
	else if (nextc == '|') {
	  if (c0 == '|')
	    takechar();
	}
      }
      /* ---- 教学里程碑 M4：+= -= *= %= 复合赋值 token 合成 ----
       * 从 '+','-','*','%' 侧起步判定（若从 '=' 侧判定会把 "x=-x" 吞成
       * "=-"，打爆 M2 一元减）。c0 为四则符且紧跟 '=' 时合成双字符 token；
       * 旧语法（四则符后不跟 '='）token 流零变化。'/=' 不在 M4 范围：
       * '/' 走下方注释分支前置判定，x/=2 仍按 '/' '=' 解析并在语法层
       * error（纪律#2，不静默）。 */
      else if ((c0 == '+') | (c0 == '-') | (c0 == '*') | (c0 == '%')) {
	takechar();
	if (nextc == '=')
	  takechar();
	/* ---- 教学里程碑 M8：++ -- 自增自减 token 合成 ----
	 * '+'/'-' 紧跟同字符时合成 '++'/'--'（其余不变，如 x=-y 仍是 '-' 'y'）。 */
	else if ((c0 == '+') | (c0 == '-')) {
	  if (nextc == c0)
	    takechar();
	}
      }
    }
    if (i == 0) {
      if (nextc == 39) {
	takechar();
	/* v0.32 F-3：字符字面量读取加 EOF 守卫（C 子集无 break，用标志变量）。
	 * 此前 `while(nextc!=39) takechar()` 对未闭合输入到 EOF 仍死循环 → token 无限增长自噬。 */
	str_done = 0;
	while (str_done == 0) {
	  if (nextc == 0 - 1) str_done = 1;
	  else if (nextc == 39) str_done = 1;
	  else takechar();
	}
	if (nextc == 39) takechar();
      }
      else if (nextc == '"') {
	takechar();
	/* v0.32 F-3：字符串字面量读取加 EOF 守卫（同上，见 cc500.c:106 原 `while(nextc!='"')`）。 */
	str_done = 0;
	while (str_done == 0) {
	  if (nextc == 0 - 1) str_done = 1;
	  else if (nextc == '"') str_done = 1;
	  else takechar();
	}
	if (nextc == '"') takechar();
      }
      else if (nextc == '/') {
	takechar();
	if (nextc == '*') {
	  nextc = getchar();
	  /* v0.32 BUG-048：块注释未闭合读至 EOF 死循环，加 EOF 守卫（复用 str_done 标志）。 */
	  str_done = 0;
	  while ((str_done == 0) & (nextc != '/')) {
	    while ((str_done == 0) & (nextc != '*')) {
	      nextc = getchar();
	      if (nextc == 0 - 1) str_done = 1;
	    }
	    if (str_done == 0) {
	      nextc = getchar();
	      if (nextc == 0 - 1) str_done = 1;
	    }
	  }
	  if (str_done == 0)
	    nextc = getchar();
	  w = 1;
	}
      }
      else if (nextc != 0-1)
	takechar();
    }
    token[i] = 0;
  }
}

int peek(char *s)
{
  int i = 0;
  while ((s[i] == token[i]) & (s[i] != 0))
    i = i + 1;
  return s[i] == token[i];
}

int accept(char *s)
{
  if (peek(s)) {
    get_token();
    return 1;
  }
  else
    return 0;
}

void expect(char *s)
{
  if (accept(s) == 0)
    error();
}

char *code;
int code_size;
int codepos;
int code_offset;
/* v0.34 M4：入口 stub call 须指向「第一个函数」。be_start 按「stub 后即首函数」
 * 假设计算 rel32；但顶层全局变量（如 `int g;`）会在 stub 后先 emit 4 字节存储，
 * 使首函数被推迟，be_start 的 rel32 便落在全局存储上→入口跳到数据→运行即挂。
 * 故在 program() 首个函数体开始处把入口 call 重定位到该函数（函数在前的旧源
 * 重算值与 be_start 相同，零改动；全局在前者修复入口跳转）。 */
int entry_call_done;

void save_int(char *p, int n)
{
  p[0] = n;
  p[1] = n >> 8;
  p[2] = n >> 16;
  p[3] = n >> 24;
}

int load_int(char *p)
{
  return ((p[0] & 255) + ((p[1] & 255) << 8) +
	  ((p[2] & 255) << 16) + ((p[3] & 255) << 24));
}

void emit(int n, char *s)
{
  i = 0;
  if (code_size <= codepos + n) {
    int x = (codepos + n) << 1;
    code = my_realloc(code, code_size, x);
    code_size = x;
  }
  while (i <= n - 1) {
    code[codepos] = s[i];
    codepos = codepos + 1;
    i = i + 1;
  }
}

void be_push()
{
  emit(1, "\x50"); /* push %eax */
}

void be_pop(int n)
{
  emit(6, "\x81\xc4...."); /* add $(n * 4),%esp */
  save_int(code + codepos - 4, n << 2);
}

char *table;
int table_size;
int table_pos;
int stack_pos;

int sym_lookup(char *s)
{
  int t = 0;
  int current_symbol = 0;
  while (t <= table_pos - 1) {
    i = 0;
    while ((s[i] == table[t]) & (s[i] != 0)) {
      i = i + 1;
      t = t + 1;
    }
    if (s[i] == table[t])
      current_symbol = t;
    while (table[t] != 0)
      t = t + 1;
    t = t + 6;
  }
  return current_symbol;
}

void sym_declare(char *s, int type, int value)
{
  int t = table_pos;
  i = 0;
  while (s[i] != 0) {
    if (table_size <= t + 10) {
      int x = (t + 10) << 1;
      table = my_realloc(table, table_size, x);
      table_size = x;
    }
    table[t] = s[i];
    i = i + 1;
    t = t + 1;
  }
  table[t] = 0;
  table[t + 1] = type;
  save_int(table + t + 2, value);
  table_pos = t + 6;
}

int sym_declare_global(char *s)
{
  int current_symbol = sym_lookup(s);
  if (current_symbol == 0) {
    sym_declare(s, 'U', code_offset);
    current_symbol = table_pos - 6;
  }
  return current_symbol;
}

void sym_define_global(int current_symbol)
{
  int i;
  int j;
  int t = current_symbol;
  int v = codepos + code_offset;
  if (table[t + 1] != 'U')
    error(); /* symbol redefined */
  i = load_int(table + t + 2) - code_offset;
  while (i) {
    j = load_int(code + i) - code_offset;
    save_int(code + i, v);
    i = j;
  }
  table[t + 1] = 'D';
  save_int(table + t + 2, v);
}

int number_of_args;

void sym_get_value(char *s)
{
  int t;
  if ((t = sym_lookup(s)) == 0)
    error();
  emit(5, "\xb8...."); /* mov $n,%eax */
  save_int(code + codepos - 4, load_int(table + t + 2));
  if (table[t + 1] == 'D') { /* defined global */
  }
  else if (table[t + 1] == 'U') /* undefined global */
    save_int(table + t + 2, codepos + code_offset - 4);
  else if (table[t + 1] == 'L') { /* local variable */
    int k = (stack_pos - table[t + 2] - 1) << 2;
    emit(7, "\x8d\x84\x24...."); /* lea (n * 4)(%esp),%eax */
    save_int(code + codepos - 4, k);
  }
  else if (table[t + 1] == 'A') { /* argument */
    int k = (stack_pos + number_of_args - table[t + 2] + 1) << 2;
    emit(7, "\x8d\x84\x24...."); /* lea (n * 4)(%esp),%eax */
    save_int(code + codepos - 4, k);
  }
  else
    error();
}

/* v0.27: 生成 ELF 头（链接基址 0x800A0000，mini-os ABI）+ 唯一 stub syscall3。
 * 布局：0x00 起 ELF 头+程序头+p_align；0x54 入口 stub（首指令）；0x5E 处 call；
 * 随后 syscall3 stub；之后（be_start 返回时 codepos 处）才是源文件里第一个函数。
 * v0.29（BUG-032）：入口 stub 在 call 前把内核 [esp+4]=argc、[esp+8]=argv 编组成
 *   首函数形参——按 cc500 约定"首参 8(%esp)/末参 4(%esp)"对齐，故首函数须声明为
 *   (char *argv, int argc)（cc500.c 即如此）。序列：
 *   mov eax,[esp+8] ; push eax      ; argv -> [esp+8]
 *   mov eax,[esp+8] ; push eax      ; argc -> [esp+4]
 *   call <首函数>; mov %eax,%ebx; xor %eax,%eax; int $0x80（SYS_EXIT=返回值）
 * 旧桩为裸 call（不编组参数），自编译产物 exec 带 argv 时静默丢参（BUG-032）。 */
void be_start()
{
  emit(16, "\x7f\x45\x4c\x46\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00");
  /* e_type=ET_EXEC e_machine=EM_386 e_version=1 e_entry=0x800A0054 e_phoff=0x34 */
  emit(16, "\x02\x00\x03\x00\x01\x00\x00\x00\x54\x00\x0a\x80\x34\x00\x00\x00");
  /* e_shoff=0 e_flags=0 e_ehsize=0x34 e_phentsize=0x20 e_phnum=1 */
  emit(16, "\x00\x00\x00\x00\x00\x00\x00\x00\x34\x00\x20\x00\x01\x00\x00\x00");
  /* ph: p_type=PT_LOAD p_offset=0 p_vaddr=0x800A0000 */
  emit(16, "\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0a\x80");
  /* p_paddr=0x800A0000 p_filesz/memsz 由 be_finish 回填 p_flags=RWX */
  emit(16, "\x00\x00\x0a\x80\x10\x4b\x00\x00\x10\x4b\x00\x00\x07\x00\x00\x00");
  /* p_align=0x1000；入口 stub（见上注释），call 的 rel32 由 save_int(code+95) 回填 */
  emit(16, "\x00\x10\x00\x00\x8b\x44\x24\x08\x50\x8b\x44\x24\x08\x50\xe8\x00");
  emit(9,  "\x00\x00\x00\x89\xc3\x31\xc0\xcd\x80");

  /* int syscall3(int n,int a,int b,int c)
   * CC500 调用方从左到右压参，故首个参数 n 位于 16(%esp)：
   *   mov 16(%esp),%eax ; mov 12(%esp),%ebx ; mov 8(%esp),%ecx
   *   mov 4(%esp),%edx  ; int $0x80 ; ret */
  sym_define_global(sym_declare_global("syscall3"));
  emit(4, "\x8b\x44\x24\x10");
  emit(4, "\x8b\x5c\x24\x0c");
  emit(4, "\x8b\x4c\x24\x08");
  emit(4, "\x8b\x54\x24\x04");
  emit(2, "\xcd\x80");
  emit(1, "\xc3");

  save_int(code + 95, codepos - 99); /* entry stub 的 call rel32 -> 首函数 */
}

void be_finish()
{
  /* v0.32 F-2：遍历全局符号表，发现仍有"未定义但被引用"的 'U' 符号 →
   * 之前无人回填，调用目标恒留 code_offset(0x800A0000)，产物 call 自己的 ELF 头，
   * 运行即 PAGE FAULT。此处收尾报错而非静默编出废产物。
   * 布局与 sym_lookup 同款：t+1=class、t+2=value，符号间 stride 6。
   * "chain 非空"判据：value 曾被 sym_get_value 改写为引用点，故 != 初始 code_offset。 */
  int t = 0;
  int bad = 0;
  while (t <= table_pos - 1) {
    /* 符号表锚点 = 名字 NUL 位置（同 sym_define_global 语义：t+1=class、t+2=value）。
     * 自符号起点起跳过名字到 NUL，再检测，t=t+6 跳到下一符号起点。 */
    while (table[t] != 0)
      t = t + 1;
    if ((table[t + 1] == 'U') & (load_int(table + t + 2) != code_offset))
      bad = 1;
    t = t + 6;
  }
  if (bad != 0) {
    sys_print("cc500: undefined symbol\x0a");
    error();
  }
  save_int(code + 68, codepos);
  save_int(code + 72, codepos);
  i = 0;
  while (i <= codepos - 1) {
    putchar(code[i]);
    i = i + 1;
  }
}

void promote(int type)
{
  /* 1 = char lval, 2 = int lval, 3 = other */
  if (type == 1)
    emit(3, "\x0f\xbe\x00"); /* movsbl (%eax),%eax */
  else if (type == 2)
    emit(2, "\x8b\x00"); /* mov (%eax),%eax */
}

int expression();

/*
 * primary-expr:
 *     identifier
 *     constant
 *     ( expression )
 */
int primary_expr()
{
  int type;
  if (('0' <= token[0]) & (token[0] <= '9')) {
    int n = 0;
    i = 0;
    /* ---- 教学里程碑 M9：十六进制字面量 0x…（2026-09-07）----
     * tokenizer 把 0x10 合为单 token（'x' 属 a-z），此处分流解析：
     * 0x 前缀按下述 hex 循环（0-9 a-f 逐位、左移 4），否则走原十进制路径
     * （含 BUG-049 的逐字符校验）。原十进制路径字节/语义零变化；
     * 非法 hex 串（0x1z、0x-1）由 error() 拒绝，不静默算错。 */
    if ((token[0] == '0') & (token[1] == 'x')) {
      i = 2;
      while (token[i]) {
	if (('0' <= token[i]) & (token[i] <= '9'))
	  n = (n << 4) + token[i] - '0';
	else if (('a' <= token[i]) & (token[i] <= 'f'))
	  n = (n << 4) + token[i] - 'a' + 10;
	else
	  error();
	i = i + 1;
      }
    }
    else {
      while (token[i]) {
	if ((token[i] < '0') | ('9' < token[i]))
	  error();
	n = (n << 1) + (n << 3) + token[i] - '0';
	i = i + 1;
      }
    }
    emit(5, "\xb8...."); /* mov $x,%eax */
    save_int(code + codepos - 4, n);
    type = 3;
  }
  else if (('a' <= token[0]) & (token[0] <= 'z')) {
    sym_get_value(token);
    type = 2;
  }
  else if (accept("(")) {
    type = expression();
    if (peek(")") == 0)
      error();
  }
  else if ((token[0] == 39) & (token[1] != 0) &
	   (token[2] == 39) & (token[3] == 0)) {
    emit(5, "\xb8...."); /* mov $x,%eax */
    save_int(code + codepos - 4, token[1]);
    type = 3;
  }
  else if (token[0] == '"') {
    int i = 0;
    int j = 1;
    int k;
    /* v0.32 F-3：解码时对 token 加 NUL 守卫——原 `while(token[j]!='"')` 无终点保护，
     * 越过 token 尾部 NUL 越界读直到堆里偶遇 '"'，且 token[i]= 写越界砸 brk arena。
     * 命中 NUL（EOF 前未闭合）→ 干净报错而非自噬。 */
    while ((token[j] != '"') & (token[j] != 0)) {
      if ((token[j] == 92) & (token[j + 1] == 'x')) {
	if (token[j + 2] <= '9')
	  k = token[j + 2] - '0';
	else
	  k = token[j + 2] - 'a' + 10;
	k = k << 4;
	if (token[j + 3] <= '9')
	  k = k + token[j + 3] - '0';
	else
	  k = k + token[j + 3] - 'a' + 10;
	token[i] = k;
	j = j + 4;
      }
      /* v0.32 F-1：\n / \t 常规转义解码（此前只解 \xNN，故源码一律写 \x0a） */
      else if ((token[j] == 92) & (token[j + 1] == 'n')) {
	token[i] = 10;
	j = j + 2;
      }
      else if ((token[j] == 92) & (token[j + 1] == 't')) {
	token[i] = 9;
	j = j + 2;
      }
      else {
	token[i] = token[j];
	j = j + 1;
      }
      i = i + 1;
    }
    if (token[j] != '"') {
      sys_print("cc500: bad string\x0a");
      error();
    }
    token[i] = 0;
    /* call ... ; the string ; pop %eax */
    emit(5, "\xe8....");
    save_int(code + codepos - 4, i + 1);
    emit(i + 1, token);
    emit(1, "\x58");
    type = 3;
  }
  else
    error();
  get_token();
  return type;
}

void binary1(int type)
{
  promote(type);
  be_push();
  stack_pos = stack_pos + 1;
}

int binary2(int type, int n, char *s)
{
  promote(type);
  emit(n, s);
  stack_pos = stack_pos - 1;
  return 3;
}

/* ---- 教学里程碑 M4：复合赋值 += -= *= %=（2026-09-06）----
 * lv op= rhs 复用两套现成机制：算术复用 binary2（emit 字节与对应二元运算
 * 完全一致）；存回复用 expression() '=' 的 lvalue 地址保持路径（push 地址 ->
 * 求值 -> pop;mov (%ebx)）。单遍下 lvalue 只求值一次：地址由 bitwise_or_expr()
 * 求得后全程驻留栈顶，rhs 内再触发下标重算（如 a[f()] += x 的 f 副作用）
 * 不会导致地址二次求值。纪律#2：非 lvalue 目标（type 3，如 3 += x）走
 * error() 绝不静默。栈纪律：进函数时 stack_pos=S，push 地址 S+1 -> 载值/
 * 回推地址净零 -> binary1 值入栈 S+2 -> binary2 弹栈 S+1 -> store 弹栈 S。 */
int compound_assign(int type, int n, char *s)
{
  if ((type != 1) & (type != 2))
    error();
  be_push();                   /* push %eax —— 保存 lv 地址（store 时复用） */
  stack_pos = stack_pos + 1;
  emit(3, "\x5b\x8b\x03");     /* pop %ebx ; mov (%ebx),%eax —— lv 当前值 -> %eax */
  emit(1, "\x53");             /* push %ebx —— 地址放回栈顶 */
  binary1(3);                  /* push %eax —— lv 值入栈（type 3 为值，promote 空操作） */
  binary2(expression(), n, s); /* %eax = lv值 op rhs（rhs 独立求值一次） */
  if (type == 2)
    emit(3, "\x5b\x89\x03");   /* pop %ebx ; mov %eax,(%ebx) —— 与 '=' 同款 store */
  else
    emit(3, "\x5b\x88\x03");   /* pop %ebx ; mov %al,(%ebx) */
  stack_pos = stack_pos - 1;
  return 3;
}

/* ---- 教学里程碑 M8：前缀/后缀自增自减 ++ --（2026-09-06）----
 * 复用 M4 compound_assign 的 lvalue 地址保持路径（push 地址->载值->运算->store）。
 *  - 前缀 ++lv/--lv：lv=lv±1，值=**新值**（发射与 `lv += 1` 一致，eax 即新值）。
 *  - 后缀 lv++/lv--：值=**旧值**，故 store 前先把旧值暂存到 %ecx（单表达式内无调用，
 *    ecx 安全；不新增 push/pop），运算存回后再 mov %ecx,%eax 恢复旧值。
 * 纪律：非 lvalue（type 3，如 3++）走 error 不静默（对齐 compound_assign）。 */
int pre_incdec(int type, int op)
{
  if ((type != 1) & (type != 2))
    error();
  be_push();                   /* push %eax —— 保存 lv 地址 */
  stack_pos = stack_pos + 1;
  emit(3, "\x5b\x8b\x03");     /* pop %ebx ; mov (%ebx),%eax —— lv 旧值 */
  emit(1, "\x53");             /* push %ebx —— 地址放回栈顶 */
  binary1(3);                  /* push %eax —— 旧值入栈 */
  emit(5, "\xb8\x01\x00\x00\x00"); /* mov $1,%eax */
  if (op == '+')
    emit(3, "\x5b\x01\xd8");           /* pop %ebx ; add %ebx,%eax -> 新值 */
  else
    emit(5, "\x5b\x29\xc3\x89\xd8");   /* pop %ebx ; sub %eax,%ebx ; mov %ebx,%eax -> 新值 */
  if (type == 2)
    emit(3, "\x5b\x89\x03");   /* pop %ebx ; mov %eax,(%ebx) */
  else
    emit(3, "\x5b\x88\x03");   /* pop %ebx ; mov %al,(%ebx) */
  stack_pos = stack_pos - 1;
  return 3;                    /* 值 = 新值，留在 eax */
}

int post_incdec(int type, int op)
{
  if ((type != 1) & (type != 2))
    error();
  be_push();                   /* push %eax —— 保存 lv 地址 */
  stack_pos = stack_pos + 1;
  emit(3, "\x5b\x8b\x03");     /* pop %ebx ; mov (%ebx),%eax —— lv 旧值 */
  emit(2, "\x89\xc1");         /* mov %eax,%ecx —— 暂存旧值（后缀返回值） */
  emit(1, "\x53");             /* push %ebx —— 地址放回栈顶 */
  if (op == '+')
    emit(3, "\x83\xc0\x01");   /* add $1,%eax -> 新值 */
  else
    emit(3, "\x83\xe8\x01");   /* sub $1,%eax -> 新值 */
  if (type == 2)
    emit(3, "\x5b\x89\x03");   /* pop %ebx ; mov %eax,(%ebx) —— 存回新值 */
  else
    emit(3, "\x5b\x88\x03");   /* pop %ebx ; mov %al,(%ebx) */
  emit(2, "\x89\xc8");         /* mov %ecx,%eax —— 恢复旧值（后缀值=旧） */
  stack_pos = stack_pos - 1;
  return 3;
}

/*
 * postfix-expr:
 *         primary-expr
 *         postfix-expr [ expression ]
 *         postfix-expr ( expression-list-opt )
 *         postfix-expr ++
 *         postfix-expr --
 */
int postfix_expr()
{
  int type = primary_expr();
  if (accept("[")) {
    binary1(type); /* pop %ebx ; add %ebx,%eax */
    binary2(expression(), 3, "\x5b\x01\xd8");
    expect("]");
    type = 1;
  }
  else if (accept("(")) {
    int s = stack_pos;
    be_push();
    stack_pos = stack_pos + 1;
    if (accept(")") == 0) {
      promote(expression());
      be_push();
      stack_pos = stack_pos + 1;
      while (accept(",")) {
	promote(expression());
	be_push();
	stack_pos = stack_pos + 1;
      }
      expect(")");
    }
    emit(7, "\x8b\x84\x24...."); /* mov (n * 4)(%esp),%eax */
    save_int(code + codepos - 4, (stack_pos - s - 1) << 2);
    emit(2, "\xff\xd0"); /* call *%eax */
    be_pop(stack_pos - s);
    stack_pos = s;
    type = 3;
  }
  else if (accept("++")) {           /* 后缀 lv++：值=旧值 */
    return post_incdec(type, '+');
  }
  else if (accept("--")) {           /* 后缀 lv--：值=旧值 */
    return post_incdec(type, '-');
  }
  return type;
}

/* ---- 教学里程碑 M2：一元 - ! ~（2026-09-06）----
 * 标准 C 优先级：postfix > unary > multiplicative…，故 unary 夹在 postfix 与
 * additive 之间；对非前缀 token 直接透传 postfix_expr()（透传路径与旧一致，保证
 * 旧语法产物字节不变）。操作数若是 lval（type 1/2）须先 promote 装载再运算。
 * 教学点：一元减与二元减共享 token '-'，靠"先试前缀、失败回退"消歧——这是
 * 递归下降处理同一 token 不同文法的经典手法。 */
int unary_expr()
{
  int type;
  if (accept("++")) {               /* 前缀 ++lv：lv=lv+1，值=新值 */
    type = unary_expr();
    return pre_incdec(type, '+');
  }
  if (accept("--")) {               /* 前缀 --lv：lv=lv-1，值=新值 */
    type = unary_expr();
    return pre_incdec(type, '-');
  }
  if (accept("-")) {
    type = unary_expr();
    promote(type);
    emit(2, "\xf7\xd8");            /* neg %eax */
    return 3;
  }
  if (accept("!")) {
    type = unary_expr();
    promote(type);
    emit(2, "\x85\xc0");            /* test %eax,%eax */
    emit(6, "\x0f\x94\xc0\x0f\xb6\xc0"); /* sete %al ; movzbl %al,%eax */
    return 3;
  }
  if (accept("~")) {
    type = unary_expr();
    promote(type);
    emit(2, "\xf7\xd0");            /* not %eax */
    return 3;
  }
  return postfix_expr();
}

/* ---- 教学里程碑 M3：乘法/除法/取模 * / %（2026-09-06）----
 * 标准 C 优先级：unary > multiplicative > additive。乘除模共用 idiv 路径：
 * 商在 eax、余数在 edx（% 用 mov %edx,%eax 取余）。C99 语义：商向零截断、
 * 余数符号随被除数（x86 idiv 天然一致，与 minicc/gcc 同口径）。
 * 教学点：除零与 INT_MIN/-1 属 UB（不产陷阱，与宿主 C 一致，由使用者负责）。 */
int multiplicative_expr()
{
  int type = unary_expr();
  while (1) {
    if (accept("*")) {
      binary1(type); /* pop %ebx ; imul %ebx,%eax */
      type = binary2(unary_expr(), 4, "\x5b\x0f\xaf\xc3");
    }
    else if (accept("/")) {
      binary1(type); /* pop %ebx ; xchg %eax,%ebx ; cdq ; idiv %ebx */
      type = binary2(unary_expr(), 6, "\x5b\x87\xd8\x99\xf7\xfb");
    }
    else if (accept("%")) {
      binary1(type); /* idiv 后取余数：mov %edx,%eax */
      type = binary2(unary_expr(), 8, "\x5b\x87\xd8\x99\xf7\xfb\x89\xd0");
    }
    else
      return type;
  }
}

/*
 * additive-expr:
 *         postfix-expr
 *         additive-expr + postfix-expr
 *         additive-expr - postfix-expr
 */
int additive_expr()
{
  int type = multiplicative_expr();
  while (1) {
    if (accept("+")) {
      binary1(type); /* pop %ebx ; add %ebx,%eax */
      type = binary2(multiplicative_expr(), 3, "\x5b\x01\xd8");
    }
    else if (accept("-")) {
      binary1(type); /* pop %ebx ; sub %eax,%ebx ; mov %ebx,%eax */
      type = binary2(multiplicative_expr(), 5, "\x5b\x29\xc3\x89\xd8");
    }
    else
      return type;
  }
}

/*
 * shift-expr:
 *         additive-expr
 *         shift-expr << additive-expr
 *         shift-expr >> additive-expr
 */
int shift_expr()
{
  int type = additive_expr();
  while (1) {
    if (accept("<<")) {
      binary1(type); /* mov %eax,%ecx ; pop %eax ; shl %cl,%eax */
      type = binary2(additive_expr(), 5, "\x89\xc1\x58\xd3\xe0");
    }
    else if (accept(">>")) {
      binary1(type); /* mov %eax,%ecx ; pop %eax ; sar %cl,%eax */
      type = binary2(additive_expr(), 5, "\x89\xc1\x58\xd3\xf8");
    }
    else
      return type;
  }
}

/*
 * relational-expr:
 *         shift-expr
 *         relational-expr <= shift-expr
 */
int relational_expr()
{
  int type = shift_expr();
  /* v0.32 F-1：关系运算原只有 <=。补齐 < / > / >=（与 <= 同构，仅 setcc 字节不同：
   * setle=0x9e setl=0x9c setge=0x9d setg=0x9f；操作数序同 <=，基于 objdump 实测确认）。 */
  while (1) {
    if (accept("<=")) {
      binary1(type);
      /* pop %ebx ; cmp %eax,%ebx ; setle %al ; movzbl %al,%eax */
      type = binary2(shift_expr(),
		   9, "\x5b\x39\xc3\x0f\x9e\xc0\x0f\xb6\xc0");
    }
    else if (accept("<")) {
      binary1(type);
      /* setl %al */
      type = binary2(shift_expr(),
		   9, "\x5b\x39\xc3\x0f\x9c\xc0\x0f\xb6\xc0");
    }
    else if (accept(">=")) {
      binary1(type);
      /* setge %al */
      type = binary2(shift_expr(),
		   9, "\x5b\x39\xc3\x0f\x9d\xc0\x0f\xb6\xc0");
    }
    else if (accept(">")) {
      binary1(type);
      /* setg %al */
      type = binary2(shift_expr(),
		   9, "\x5b\x39\xc3\x0f\x9f\xc0\x0f\xb6\xc0");
    }
    else
      return type;
  }
}

/*
 * equality-expr:
 *         relational-expr
 *         equality-expr == relational-expr
 *         equality-expr != relational-expr
 */
int equality_expr()
{
  int type = relational_expr();
  while (1) {
    if (accept("==")) {
      binary1(type);
      /* pop %ebx ; cmp %eax,%ebx ; sete %al ; movzbl %al,%eax */
      type = binary2(relational_expr(),
		     9, "\x5b\x39\xc3\x0f\x94\xc0\x0f\xb6\xc0");
    }
    else if (accept("!=")) {
      binary1(type);
      /* pop %ebx ; cmp %eax,%ebx ; setne %al ; movzbl %al,%eax */
      type = binary2(relational_expr(),
		     9, "\x5b\x39\xc3\x0f\x95\xc0\x0f\xb6\xc0");
    }
    else
      return type;
  }
}

/*
 * bitwise-and-expr:
 *         equality-expr
 *         bitwise-and-expr & equality-expr
 */
int bitwise_and_expr()
{
  int type = equality_expr();
  while (accept("&")) {
    binary1(type); /* pop %ebx ; and %ebx,%eax */
    type = binary2(equality_expr(), 3, "\x5b\x21\xd8");
  }
  return type;
}

/* ---- 教学里程碑 M2：位异或 ^（2026-09-06）----
 * 标准 C 位运算优先级：& > ^ > |。cc500 原只有 & 与 | 两层（& 直接挂 | 下），
 * 此处按标准插入 ^ 层：| 调 ^、^ 调 &。与 & / | 发射同构，仅 opcode 不同。 */
int bitxor_expr()
{
  int type = bitwise_and_expr();
  while (accept("^")) {
    binary1(type); /* pop %ebx ; xor %ebx,%eax */
    type = binary2(bitwise_and_expr(), 3, "\x5b\x31\xd8");
  }
  return type;
}

/*
 * bitwise-or-expr:
 *         bitwise-and-expr
 *         bitwise-and-expr | bitwise-or-expr
 */
int bitwise_or_expr()
{
  int type = bitxor_expr();
  while (accept("|")) {
    binary1(type); /* pop %ebx ; or %ebx,%eax */
    type = binary2(bitxor_expr(), 3, "\x5b\x09\xd8");
  }
  return type;
}

/* ---- 教学里程碑 M6：短路逻辑 && / ||（2026-09-06）----
 * 优先级：|| < && < | < ^ < &（C 标准）。所以在 expression() 与 bitwise_or_expr()
 * 之间插入两层 logical_or_expr -> logical_and_expr -> bitwise_or_expr；语料无 &&/||
 * 时全程纯透传 → 既有源码产物字节零变化（P1==P2 不受影响；cc500.c 自身也不用 &&/||，
 * 仍按位运算 & | 写布尔守卫，见 OBS-CC-3）。token 由 get_token 已合成独立 &&/||。
 * 短路必须用跳转而非位运算（& | 会贪心求右操作数、也不归一到 0/1）：
 *   a && b：a==0 → 略 b、结果 0；否则算 b，全非零 → 1（test;je 双跳 + mov 0/1）
 *   a || b：a!=0 → 略 b、结果 1；否则算 b，b!=0 → 1、b==0 → 0（test;jne 双跳 +
 *   mov 0/1）。未决前向跳用 codepos + save_int 回填（与 if/while 同机制，不新增 emit
 *   原语）；左结合靠循环：每轮左端结果已在 eax（值型 3），promote 为 no-op 直接再判。 */
int logical_and_expr()
{
  int type;
  int je1;
  int je2;
  int jmp;
  int zero;
  type = bitwise_or_expr();
  while (accept("&&")) {
    promote(type);                       /* 左操作数载值进 eax（type 3 = 值，no-op） */
    emit(8, "\x85\xc0\x0f\x84....");     /* test %eax ; je L_zero —— 左=0 提前得 0 */
    je1 = codepos;
    type = bitwise_or_expr();            /* 右操作数 */
    promote(type);                       /* 载值 */
    emit(8, "\x85\xc0\x0f\x84....");     /* test %eax ; je L_zero —— 右=0 也得 0 */
    je2 = codepos;
    emit(5, "\xb8\x01\x00\x00\x00");     /* mov $1,%eax —— 左右皆非零 */
    emit(5, "\xe9....");                 /* jmp L_end（越过零值） */
    jmp = codepos;
    zero = codepos;                      /* L_zero：mov $0 起点 */
    save_int(code + je1 - 4, zero - je1);
    save_int(code + je2 - 4, zero - je2);
    emit(5, "\xb8\x00\x00\x00\x00");     /* mov $0,%eax */
    save_int(code + jmp - 4, codepos - jmp);
    type = 3;                            /* 结果恒为 0/1 布尔值 */
  }
  return type;
}

int logical_or_expr()
{
  int type;
  int jn1;
  int jn2;
  int jmp;
  int one;
  type = logical_and_expr();
  while (accept("||")) {
    promote(type);                       /* 左载值 */
    emit(8, "\x85\xc0\x0f\x85....");     /* test %eax ; jne L_one —— 左非零提前得 1 */
    jn1 = codepos;
    type = logical_and_expr();           /* 右 */
    promote(type);
    emit(8, "\x85\xc0\x0f\x85....");     /* test %eax ; jne L_one —— 右非零得 1 */
    jn2 = codepos;
    emit(5, "\xb8\x00\x00\x00\x00");     /* mov $0,%eax —— 全假 */
    emit(5, "\xe9....");                 /* jmp L_end（越过真值） */
    jmp = codepos;
    one = codepos;                       /* L_one：mov $1 起点 */
    save_int(code + jn1 - 4, one - jn1);
    save_int(code + jn2 - 4, one - jn2);
    emit(5, "\xb8\x01\x00\x00\x00");     /* mov $1,%eax */
    save_int(code + jmp - 4, codepos - jmp);
    type = 3;
  }
  return type;
}

/*
 * expression:
 *         conditional-expr
 *         conditional-expr = expression
 * conditional-expr:
 *         logical-or-expr
 *         logical-or-expr ? expression : conditional-expr
 */
int expression()
{
  int type;
  cc_depth = cc_depth + 1;
  if (cc_depth > 512)
    error();               /* OBS-CC-1：深嵌套注入（((((... 或 a=b=c=...) 在耗尽栈前被拒绝 */
  type = logical_or_expr();
  /* ---- 教学里程碑 M7：?: 三目（2026-09-06）----
   * C 优先级：三元 ?: 高于赋值 =、低于逻辑或 ||，故插在 logical_or 之后、'=' 之前。
   * 单遍无 AST：条件 test;je 跳假分支、真分支后 jmp 跳过假分支，前向跳用 codepos+save_int
   * 回填（与 if/while/短路同机制，不新增 emit 原语）。条件与两分支各求值一次；结果恒为
   * 值（promote 载值），故可作为赋值 RHS、但不能当赋值目标。缺 ':' 即错误（不静默）。
   * 语料无 '?' 时本分支不触发 → 旧源码 emit 字节零变化。 */
  if (accept("?")) {
    int p_else;
    int p_jmp;
    promote(type);                       /* 条件载值进 eax（type 3 为 no-op） */
    emit(8, "\x85\xc0\x0f\x84....");     /* test %eax ; je L_else */
    p_else = codepos;
    type = expression();                 /* 真分支（可为任意表达式，含赋值/嵌套） */
    promote(type);                       /* 载值 */
    emit(5, "\xe9....");                 /* jmp L_end */
    p_jmp = codepos;
    save_int(code + p_else - 4, codepos - p_else);   /* je -> L_else（副本） */
    expect(":");
    type = expression();                 /* 假分支 */
    promote(type);
    save_int(code + p_jmp - 4, codepos - p_jmp);     /* jmp -> L_end */
    type = 3;
  }
  if (accept("=")) {
    be_push();
    stack_pos = stack_pos + 1;
    promote(expression());
    if (type == 2)
      emit(3, "\x5b\x89\x03"); /* pop %ebx ; mov %eax,(%ebx) */
    else
      emit(3, "\x5b\x88\x03"); /* pop %ebx ; mov %al,(%ebx) */
    stack_pos = stack_pos - 1;
    type = 3;
  }
  /* ---- 教学里程碑 M4：+= -= *= %= 复合赋值 ----
   * 纪律#1：原 '=' 分支零改动；复合赋值走 else-if 新增分支，旧语法（无
   * op= token）不进新分支，emit 字节保持不变。各 op emit 与
   * additive/multiplicative 层完全同款（add/sub/imul/idiv+取余）。 */
  else if (accept("+="))
    type = compound_assign(type, 3, "\x5b\x01\xd8");                     /* pop %ebx ; add %ebx,%eax */
  else if (accept("-="))
    type = compound_assign(type, 5, "\x5b\x29\xc3\x89\xd8");             /* pop %ebx ; sub %eax,%ebx ; mov %ebx,%eax */
  else if (accept("*="))
    type = compound_assign(type, 4, "\x5b\x0f\xaf\xc3");                 /* pop %ebx ; imul %ebx,%eax */
  else if (accept("%="))
    type = compound_assign(type, 8, "\x5b\x87\xd8\x99\xf7\xfb\x89\xd0"); /* pop %ebx ; xchg %eax,%ebx ; cdq ; idiv %ebx ; mov %edx,%eax */
  cc_depth = cc_depth - 1;
  return type;
}

/*
/* ---- 教学里程碑 M1 前向声明（for / do-while 实现放 statement() 之后）---- */
int stmt_for();
int stmt_do();

/*
 * type-name:
 *     char *
 *     int
 */
void type_name()
{
  get_token();
  while (accept("*")) {
  }
}

/*
 * statement:
 *     { statement-list-opt }
 *     type-name identifier ;
 *     type-name identifier = expression;
 *     if ( expression ) statement
 *     if ( expression ) statement else statement
 *     while ( expression ) statement
 *     do statement while ( expression ) ;
 *     for ( expression-opt ; expression-opt ; expression-opt ) statement
 *     break ;
 *     continue ;
 *     return ;
 *     expr ;
 */

/* ---- 教学里程碑 M5：break / continue 循环控制（2026-09-06）----
 * 无 AST 单遍发射下，break/continue 用"循环帧栈"管理未决跳转：
 *   - 进入每层循环压一帧，记录该层起始时 break/continue 挂起的"水位"（计数）；
 *   - body 内 break/continue 各自 emit `jmp` 并把指令终点位置(codepos)记入挂起池；
 *   - 循环退出 / continue 目标确定后，把本帧水位之后新增的挂起位置统一回填。
 * 帧栈与挂起池存于堆（malloc 出的 char*，4 字节手工打包）——cc500 无数组声明，
 * 仅支持指针下标（且下标按字节），故绕道堆缓冲。任意嵌套 while/do/for 的
 * break/continue 都能回填到正确目标。
 * 纪律：仅新增分支、不改任何既有发射路径；cc500.c 自身用新语法 → P1==P2 不变式成立。
 * 工程惯例：辅助函数内变量一律单声明（`int x;` 一句一个——cc500 只支持单声明）。 */
int loop_depth;
char *loop_frames;   /* 帧栈：每帧 8B=2×int（br_mark 水位, cont_mark 水位） */
char *loop_breaks;   /* 挂起 break 的 jmp 位置池（每项 4B） */
char *loop_conts;    /* 挂起 continue 的 jmp 位置池（每项 4B） */
int loop_break_cnt;  /* 挂起 break 总数（=池顶游标） */
int loop_cont_cnt;   /* 挂起 continue 总数（=池顶游标） */

/* 4 字节手工打包（与 save_int/load_ptr 同源，保证 cc500 自举可编） */
void bput4(char *b, int o, int v)
{
  b[o] = v & 255;
  b[o + 1] = (v >> 8) & 255;
  b[o + 2] = (v >> 16) & 255;
  b[o + 3] = (v >> 24) & 255;
}

int bget4(char *b, int o)
{
  return (b[o] & 255) + ((b[o + 1] & 255) << 8) +
         ((b[o + 2] & 255) << 16) + ((b[o + 3] & 255) << 24);
}

/* 进入一层循环：压帧，记录当前两池水位（作为本层 break/continue 起始标记） */
void loop_push()
{
  int fb;
  if (loop_depth >= 16)
    error();                       /* 循环嵌套过深拒绝 */
  fb = loop_depth * 8;
  bput4(loop_frames, fb, loop_break_cnt);
  bput4(loop_frames, fb + 4, loop_cont_cnt);
  loop_depth = loop_depth + 1;
}

/* body 内某次 break：只循环内合法，emit jmp 并把位置记入挂起池 */
void stmt_break()
{
  int n;
  if (loop_depth <= 0)
    error();                       /* 循环外 break */
  n = loop_break_cnt;
  if (n >= 256)
    error();                       /* 挂起池满拒绝，防堆越界 */
  emit(5, "\xe9....");             /* jmp（退出点回填） */
  bput4(loop_breaks, n * 4, codepos);
  loop_break_cnt = n + 1;
  expect(";");
}

/* body 内某次 continue：逻辑同 break，记入 continue 挂起池 */
void stmt_continue()
{
  int n;
  if (loop_depth <= 0)
    error();                       /* 循环外 continue */
  n = loop_cont_cnt;
  if (n >= 256)
    error();
  emit(5, "\xe9....");             /* jmp（continue 目标回填） */
  bput4(loop_conts, n * 4, codepos);
  loop_cont_cnt = n + 1;
  expect(";");
}

/* 循环退出点确定后，回填本帧新增的全部 break jmp 到 codepos（=退出点） */
void loop_patch_break()
{
  int fb;
  int mark;
  int i;
  int pos;
  fb = (loop_depth - 1) * 8;
  mark = bget4(loop_frames, fb);
  for (i = mark; i <= loop_break_cnt - 1; i = i + 1) {
    pos = bget4(loop_breaks, i * 4);
    save_int(code + pos - 4, codepos - pos);
  }
  loop_break_cnt = mark;           /* 回退到本层起点，交还内层空间 */
}

/* continue 目标 ct 确定后，回填本帧新增的全部 continue jmp 到 ct */
void loop_patch_continue(int ct)
{
  int fb;
  int mark;
  int i;
  int pos;
  fb = (loop_depth - 1) * 8;
  mark = bget4(loop_frames, fb + 4);
  for (i = mark; i <= loop_cont_cnt - 1; i = i + 1) {
    pos = bget4(loop_conts, i * 4);
    save_int(code + pos - 4, ct - pos);
  }
  loop_cont_cnt = mark;
}

/* 离开循环：退帧 */
void loop_pop()
{
  loop_depth = loop_depth - 1;
}

void statement()
{
  int p1;
  int p2;
  cc_depth = cc_depth + 1;
  if (cc_depth > 512)
    error();               /* OBS-CC-1：块 / if / while 深重入在耗尽栈前被拒绝 */
  if (accept("{")) {
    int n = table_pos;
    int s = stack_pos;
    while (accept("}") == 0)
      statement();
    table_pos = n;
    be_pop(stack_pos - s);
    stack_pos = s;
  }
  else if (peek("char") | peek("int")) {
    type_name();
    sym_declare(token, 'L', stack_pos);
    get_token();
    if (accept("="))
      promote(expression());
    expect(";");
    be_push();
    stack_pos = stack_pos + 1;
  }
  else if (accept("if")) {
    expect("(");
    promote(expression());
    emit(8, "\x85\xc0\x0f\x84...."); /* test %eax,%eax ; je ... */
    p1 = codepos;
    expect(")");
    statement();
    emit(5, "\xe9...."); /* jmp ... */
    p2 = codepos;
    save_int(code + p1 - 4, codepos - p1);
    if (accept("else"))
      statement();
    save_int(code + p2 - 4, codepos - p2);
  }
  else if (accept("while")) {
    expect("(");
    p1 = codepos;                  /* cond 顶：continue 目标 */
    promote(expression());
    emit(8, "\x85\xc0\x0f\x84...."); /* test %eax,%eax ; je ... */
    p2 = codepos;
    expect(")");
    loop_push();                   /* M5：压 while 帧 */
    statement();
    emit(5, "\xe9...."); /* jmp 回 cond 顶 */
    save_int(code + codepos - 4, p1 - codepos);
    loop_patch_continue(p1);       /* continue → cond 顶 */
    loop_patch_break();            /* break → 当前 codepos（退出点） */
    loop_pop();
    save_int(code + p2 - 4, codepos - p2); /* cond je → 退出点 */
  }
  else if (accept("do")) {        /* M1：do-while */
    stmt_do();
  }
  else if (accept("for")) {       /* M1：for(init;cond;step) */
    stmt_for();
  }
  else if (accept("break")) {     /* M5：break（循环内） */
    stmt_break();
  }
  else if (accept("continue")) {  /* M5：continue（循环内） */
    stmt_continue();
  }
  else if (accept("return")) {
    if (peek(";") == 0)
      promote(expression());
    expect(";");
    be_pop(stack_pos);
    emit(1, "\xc3"); /* ret */
  }
  else {
    expression();
    expect(";");
  }
  cc_depth = cc_depth - 1;
}

/* ---- 教学里程碑 M1：for / do-while 实现（2026-09-06）----
 * 纪律：仅新增分支，不改任何既有发射路径；cc500.c 自身源码不使用新语法 →
 * P1==P2 自举不动点不受影响。
 *
 * for(init;cond;step)body —— 无 AST 单遍下"step 后置"的标准解法：双跳摆渡布局。
 * 发射顺序 = 解析顺序（init,cond,step,body），执行顺序靠跳转编排为每轮
 * cond→body→step：
 *   init; L_top:<cond;test;je L_exit>; jmp L_body; L_step:<step>; jmp L_top;
 *   L_body:<body>; jmp L_step; L_exit:
 * step 区物理位于 body 前但被首条 jmp 跳过，每轮由 body 尾部 jmp 跳回执行。
 * 相比"延迟缓冲+搬移"方案：step 原地发射，内部函数调用/全局引用的 rel 天然正确，
 * 无需重定位；代价是每轮多两条 jmp（教学编译器不追求性能）。
 * 空 cond / 空 step 均按 C 语义成立：无 cond 时 L_exit 无引用（for(;;) 为无限循环）。
 */
int stmt_for()
{
  int p_top;
  int p_step;
  int p_body;
  int pj;      /* jmp body 的 rel 起点 */
  int pexit;   /* cond je 的 rel 起点；-1=无 cond */
  expect("(");
  if (peek(";") == 0)
    expression();                    /* init（可选） */
  expect(";");
  p_top = codepos;                   /* L_top */
  pexit = 0 - 1;
  if (peek(";") == 0) {              /* cond（可选） */
    promote(expression());
    emit(8, "\x85\xc0\x0f\x84...."); /* test; je L_exit（占位） */
    pexit = codepos;
  }
  expect(";");
  emit(5, "\xe9....");               /* jmp L_body（先于 step 区发射） */
  pj = codepos;
  p_step = codepos;                  /* L_step（空 step 时即 jmp L_top 起点） */
  if (peek(")") == 0)
    expression();                    /* step（可选，物理在 body 前） */
  expect(")");
  emit(5, "\xe9....");               /* jmp L_top */
  save_int(code + codepos - 4, p_top - codepos);
  p_body = codepos;                  /* L_body */
  loop_push();                       /* M5：压 for 帧 */
  statement();                       /* body */
  emit(5, "\xe9....");               /* jmp L_step（回 step 区） */
  save_int(code + codepos - 4, p_step - codepos);
  loop_patch_continue(p_step);       /* M5：continue → step */
  loop_patch_break();                /* M5：break → 当前 codepos（退出点） */
  loop_pop();                        /* M5：退 for 帧 */
  save_int(code + pj - 4, p_body - pj);      /* 回填 jmp L_body */
  if (pexit != 0 - 1)
    save_int(code + pexit - 4, codepos - pexit); /* 回填 cond je → L_exit */
  return 0;
}

/* do body while ( cond ) ; —— 与 while 同构，仅方向相反：先执行一次再判条件。
 * M5：do 的 continue 目标 = 每次回跳的 cond 求值起点（pc）；break = 整个构造后的退出点。 */
int stmt_do()
{
  int p1;
  int p2;
  int pc;      /* M5：cond 求值起点 = continue 目标 */
  p1 = codepos;                      /* body 顶 */
  loop_push();                       /* M5：压 do 帧 */
  statement();                       /* body（内可 break/continue，先记 jmp 位置） */
  if (peek("while") == 0)
    error();                         /* 缺 while 关键字（do 的 body 无独立 else，正常返回时 token 应为 while） */
  accept("while");
  expect("(");
  pc = codepos;                      /* M5：continue 在此回跳（先判 cond 再回 body 顶） */
  promote(expression());
  emit(8, "\x85\xc0\x0f\x85....");   /* test; jne body 顶（条件非零跳回） */
  p2 = codepos;
  expect(")");
  expect(";");
  save_int(code + p2 - 4, p1 - codepos);
  loop_patch_continue(pc);           /* M5：continue → cond 求值起点 */
  loop_patch_break();                /* M5：break → 当前 codepos（退出点） */
  loop_pop();                        /* M5：退 do 帧 */
  return 0;
}

/*
 * program:
 *     declaration
 *     declaration program
 *
 * declaration:
 *     type-name identifier ;
 *     type-name identifier ( parameter-list ) ;
 *     type-name identifier ( parameter-list ) statement
 *
 * parameter-list:
 *     parameter-declaration
 *     parameter-list, parameter-declaration
 *
 * parameter-declaration:
 *     type-name identifier-opt
 */
void program()
{
  int current_symbol;
  while (token[0]) {
    type_name();
    if (token[0] == 0)
      error();           /* v0.27b: 缺名字处遇 EOF（畸形输入）直接报错而非死循环 */
    current_symbol = sym_declare_global(token);
    get_token();
    if (accept(";")) {
      sym_define_global(current_symbol);
      emit(4, "\x00\x00\x00\x00");
    }
    else if (accept("(")) {
      int n = table_pos;
      number_of_args = 0;
      while (accept(")") == 0) {
	number_of_args = number_of_args + 1;
	type_name();
	if (token[0] == 0)
	  error();       /* v0.27b: 形参列表在 EOF 处未闭合（畸形输入） */
	if (peek(")") == 0) {
	  sym_declare(token, 'A', number_of_args);
	  get_token();
	}
	accept(","); /* ignore trailing comma */
      }
      if (accept(";") == 0) {
	if (entry_call_done == 0) {
	  entry_call_done = 1;
	  /* 首个函数：把入口 stub 的 call rel32 重定位到该函数起点。codepos-99
	   * 同 be_start 公式（文件偏移 codepos → 目标 vaddr=0x800A0000+codepos，
	   * call 下一条 0x63，rel32=codepos-0x63=codepos-99）。 */
	  save_int(code + 95, codepos - 99);
	}
	sym_define_global(current_symbol);
	statement();
	emit(1, "\xc3"); /* ret */
      }
      table_pos = n;
    }
    else
      error();
  }
}

/* ---- v0.27: mini-os 文件系统 I/O（代替 stdin/stdout） ----
 * 输入：整读 /cc500.c 进 in_data（32KB 上限，源文件 ~19KB 足够）；
 * 输出：putchar 为空操作，be_finish 后由 flush_output 把 code 一次性写 /out.elf。
 * mini-os 系统调用号：SYS_PRINT=1 SYS_FS_CREATE=13 SYS_FS_OPEN=14
 *   SYS_FS_WRITE=15 SYS_FS_READ=16 SYS_FS_DELETE=19 SYS_BRK=35 SYS_EXIT=0。
 * syscall3 调用约定：syscall3(n, a, b, c) -> eax=n, ebx=a, ecx=b, edx=c。
 */
int in_slot;
int in_len;
int in_pos;
char *in_data;
int in_path;    /* 输入路径（0=默认 /cc500.c），v0.27b 由 argv[1] 指定 */
int out_path;   /* 输出路径（0=默认 /out.elf），v0.27b 由 argv[2] 指定 */

/* 从 argv 指针数组按字节偏移读 4 字节指针（CC500 无 int* 解引用/类型转换，
 * 逐字节拼回小端 32 位）。argv[0] 在偏移 0、argv[1] 在偏移 4、argv[2] 在偏移 8。 */
int load_ptr(char *base, int off)
{
  return (base[off] & 255) + ((base[off + 1] & 255) << 8) +
         ((base[off + 2] & 255) << 16) + ((base[off + 3] & 255) << 24);
}

int getchar(void)
{
  int r;
  if (in_pos <= in_len - 1) {
    r = in_data[in_pos];
    in_pos = in_pos + 1;
    return r;
  }
  else
    return 0 - 1;
}

int putchar(int c)
{
  return c;   /* 输出改由 flush_output 一次性写 code 缓冲 */
}

void *malloc(int n)
{
  int old;
  old = syscall3(35, 0, 0, 0);            /* SYS_BRK：查询当前 program break */
  if (syscall3(35, old + n, 0, 0) == 0)   /* SYS_BRK：上移（0=成功），返回旧 brk */
    return old;
  else
    return 0 - 1;
}

void exit(int code)
{
  syscall3(0, code, 0, 0);                /* SYS_EXIT：内核终止进程，不返回 */
}

int sys_print(char *s)
{
  return syscall3(1, s, 0, 0);            /* SYS_PRINT */
}

/* 整读输入文件（in_path 或默认 /cc500.c）进 in_data，返回 0/-1
 * （CC500 子集无 break，用 done 标志退出；缓冲满后探 1 字节判是否被截断） */
int open_input()
{
  int n;
  int done;
  int full;
  char *p;
  p = "/cc500.c";
  if (in_path != 0)
    p = in_path;
  if (syscall3(14, 1, p, 0) != 0)            /* SYS_FS_OPEN slot1 只读 */
    return 0 - 1;
  in_data = malloc(65536);
  if (in_data == (0 - 1))
    return 0 - 1;
  in_len = 0;
  done = 0;
  full = 0;
  while (done == 0) {
    if (65536 <= in_len) { done = 1; full = 1; }  /* 缓冲满 */
    else {
      n = syscall3(16, 1, in_data + in_len, 4096);   /* SYS_FS_READ slot1 */
      if (n <= 0) done = 1;
      else in_len = in_len + n;
    }
  }
  if (full != 0) {
    n = syscall3(16, 1, in_data, 1);         /* 探 1 字节：>0 说明被截断 */
    if (n != 0) {                            /* 显式报错，而非静默编半个文件 */
      syscall3(17, 1, 0, 0);
      sys_print("cc500: input too big (>64KB)\x0a");
      return 0 - 1;
    }
  }
  syscall3(17, 1, 0, 0);                     /* SYS_FS_CLOSE slot1（fs 槽全局共享，必须还） */
  in_pos = 0;
  return 0;
}

/* 建/清输出文件（out_path 或默认 /out.elf）并打开只写（slot2），返回 0/-1 */
int setup_output()
{
  char *p;
  p = "/out.elf";
  if (out_path != 0)
    p = out_path;
  syscall3(19, p, 0, 0);                     /* SYS_FS_DELETE：清旧文件 */
  if (syscall3(13, p, 0, 0) <= 0 - 1)        /* SYS_FS_CREATE：新 inode */
    return 0 - 1;
  if (syscall3(14, 2, p, 1) != 0)            /* SYS_FS_OPEN slot2 只写 */
    return 0 - 1;
  return 0;
}

/* 把 code[0..codepos-1] 整写回 /out.elf，返回 0/-1 */
int flush_output()
{
  int n;
  n = syscall3(15, 2, code, codepos);        /* SYS_FS_WRITE slot2 */
  syscall3(17, 2, 0, 0);                     /* SYS_FS_CLOSE slot2 */
  if (n != codepos)
    return 0 - 1;
  return 0;
}

int main1(char *argv, int argc)
{
  code_offset = 2148139008; /* 0x800A0000 = APP_LINK（与内核 ELF 加载器一致） */
  in_path = 0;              /* 默认 /cc500.c */
  out_path = 0;             /* 默认 /out.elf */
  if (3 <= argc) {          /* argv[1]=输入路径 argv[2]=输出路径 */
    in_path = load_ptr(argv, 4);
    out_path = load_ptr(argv, 8);
  }
  if (open_input() != 0) {
    sys_print("cc500: input open fail\x0a");
    return 1;
  }
  if (setup_output() != 0) {
    sys_print("cc500: output setup fail\x0a");
    return 1;
  }
  /* v0.35 F-4：先分配 token 缓冲。token 为全局指针（初值 NULL），正常路径靠 takechar()
   * -> my_realloc 惰性分配；但对空/纯空白源一次 takechar 都不触发，get_token 末尾
   * `token[i]=0` 会写 NULL -> SIGSEGV(139)（而非干净报错）。预分配后空源走干净 error。 */
  token = malloc(32);
  token_size = 32;
  /* v0.36 M5：预分配 break/continue 循环帧栈堆缓冲（cc500 无数组声明，唯有指针下标）。
   * 18 层帧×8B + 两侧挂起池各 256×4B，测试源足够；越界由 stmt_break/continue 的计数守卫兜底。 */
  loop_frames = malloc(144);
  loop_breaks = malloc(1024);
  loop_conts = malloc(1024);
  be_start();
  nextc = getchar();
  get_token();
  program();
  if (entry_call_done == 0) {
    /* 无任何函数定义 -> 无入口（cc500 契约「首个定义即入口」），产出无入口 ELF 运行必挂；
     * 与 be_finish 的 undefined-symbol 纪律一致：干净报错而非编出废产物（空/纯空白/仅注释源）。 */
    sys_print("cc500: undefined symbol\x0a");
    return 1;
  }
  be_finish();
  if (flush_output() != 0) {
    sys_print("cc500: output write fail\x0a");
    return 1;
  }
  sys_print("cc500: compiled OK\x0a");
  return 0;
}
