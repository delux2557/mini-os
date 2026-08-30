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

void error()
{
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
  while (w) {
    w = 0;
    while ((nextc == ' ') | (nextc == 9) | (nextc == 10))
      nextc = getchar();
    i = 0;
    while ((('a' <= nextc) & (nextc <= 'z')) |
	   (('0' <= nextc) & (nextc <= '9')) | (nextc == '_'))
      takechar();
    if (i == 0)
      while ((nextc == '<') | (nextc == '=') | (nextc == '>') |
	     (nextc == '|') | (nextc == '&') | (nextc == '!'))
	takechar();
    if (i == 0) {
      if (nextc == 39) {
	takechar();
	while (nextc != 39)
	  takechar();
	takechar();
      }
      else if (nextc == '"') {
	takechar();
	while (nextc != '"')
	  takechar();
	takechar();
      }
      else if (nextc == '/') {
	takechar();
	if (nextc == '*') {
	  nextc = getchar();
	  while (nextc != '/') {
	    while (nextc != '*')
	      nextc = getchar();
	    nextc = getchar();
	  }
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
 * 布局：0x00 起 96B ELF 头+程序头；0x54 入口 stub；随后 syscall3 stub；
 * 之后（be_start 返回时 codepos 处）才是源文件里第一个函数 app_main。
 * 入口 stub：call app_main; mov %eax,%ebx; xor %eax,%eax; int $0x80（SYS_EXIT=0）*/
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
  /* p_align=0x1000；入口 stub（见上注释），rel32 由 save_int(code+85) 回填 */
  emit(16, "\x00\x10\x00\x00\xe8\x00\x00\x00\x00\x89\xc3\x31\xc0\xcd\x80\x00");

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

  save_int(code + 85, codepos - 89); /* entry set to first thing in file */
}

void be_finish()
{
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
    while (token[i]) {
      n = (n << 1) + (n << 3) + token[i] - '0';
      i = i + 1;
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
    while (token[j] != '"') {
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
      else {
	token[i] = token[j];
	j = j + 1;
      }
      i = i + 1;
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

/*
 * postfix-expr:
 *         primary-expr
 *         postfix-expr [ expression ]
 *         postfix-expr ( expression-list-opt )
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
  return type;
}

/*
 * additive-expr:
 *         postfix-expr
 *         additive-expr + postfix-expr
 *         additive-expr - postfix-expr
 */
int additive_expr()
{
  int type = postfix_expr();
  while (1) {
    if (accept("+")) {
      binary1(type); /* pop %ebx ; add %ebx,%eax */
      type = binary2(postfix_expr(), 3, "\x5b\x01\xd8");
    }
    else if (accept("-")) {
      binary1(type); /* pop %ebx ; sub %eax,%ebx ; mov %ebx,%eax */
      type = binary2(postfix_expr(), 5, "\x5b\x29\xc3\x89\xd8");
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
  while (accept("<=")) {
    binary1(type);
    /* pop %ebx ; cmp %eax,%ebx ; setle %al ; movzbl %al,%eax */
    type = binary2(shift_expr(),
		   9, "\x5b\x39\xc3\x0f\x9e\xc0\x0f\xb6\xc0");
  }
  return type;
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

/*
 * bitwise-or-expr:
 *         bitwise-and-expr
 *         bitwise-and-expr | bitwise-or-expr
 */
int bitwise_or_expr()
{
  int type = bitwise_and_expr();
  while (accept("|")) {
    binary1(type); /* pop %ebx ; or %ebx,%eax */
    type = binary2(bitwise_and_expr(), 3, "\x5b\x09\xd8");
  }
  return type;
}

/*
 * expression:
 *         bitwise-or-expr
 *         bitwise-or-expr = expression
 */
int expression()
{
  int type = bitwise_or_expr();
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
  return type;
}

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
 *     return ;
 *     expr ;
 */
void statement()
{
  int p1;
  int p2;
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
    p1 = codepos;
    promote(expression());
    emit(8, "\x85\xc0\x0f\x84...."); /* test %eax,%eax ; je ... */
    p2 = codepos;
    expect(")");
    statement();
    emit(5, "\xe9...."); /* jmp ... */
    save_int(code + codepos - 4, p1 - codepos);
    save_int(code + p2 - 4, codepos - p2);
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
 * （CC500 子集无 break，用 done 标志退出） */
int open_input()
{
  int n;
  int done;
  char *p;
  p = "/cc500.c";
  if (in_path != 0)
    p = in_path;
  if (syscall3(14, 1, p, 0) != 0)            /* SYS_FS_OPEN slot1 只读 */
    return 0 - 1;
  in_data = malloc(32768);
  if (in_data == (0 - 1))
    return 0 - 1;
  in_len = 0;
  done = 0;
  while (done == 0) {
    if (32768 <= in_len) done = 1;           /* 缓冲满 */
    else {
      n = syscall3(16, 1, in_data + in_len, 4096);   /* SYS_FS_READ slot1 */
      if (n <= 0) done = 1;
      else in_len = in_len + n;
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
  be_start();
  nextc = getchar();
  get_token();
  program();
  be_finish();
  if (flush_output() != 0) {
    sys_print("cc500: output write fail\x0a");
    return 1;
  }
  sys_print("cc500: compiled OK\x0a");
  return 0;
}
