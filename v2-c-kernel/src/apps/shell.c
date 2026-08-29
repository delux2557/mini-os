/* mini-os/v2-c-kernel/src/apps/shell.c
 * 交互式 Shell（v0.9）：从文件系统加载 ELF 应用并运行的"用户程序"。
 *  - 独立编译链接到 0x80000000，由内核在启动时从 fs 加载
 *  - 阻塞式 readline 读命令；命令：help / ls / cat / run / exit
 *  - run <prog> 通过 sys_spawn_file 启动应用，再 sys_wait 等待其退出
 */
#include "user_lib.h"

#define CMD_MAX  128
#define ARG_MAX  32

/* 从整行中取第一个单词到 out，返回是否非空 */
static int split_cmd(char *line, char *out) {
    uint32_t i = 0;
    while (line[i] == ' ') i++;                 /* 跳过前导空格 */
    uint32_t j = 0;
    while (line[i] && line[i] != ' ' && j < ARG_MAX - 1) out[j++] = line[i++];
    out[j] = 0;
    return j > 0;
}

/* 取命令后的参数（第一个单词之后剩余，去首尾空格） */
static void split_arg(char *line, char *out) {
    uint32_t i = 0;
    while (line[i] == ' ') i++;
    while (line[i] && line[i] != ' ') i++;      /* 跳过命令单词 */
    while (line[i] == ' ') i++;
    uint32_t j = 0;
    while (line[i] && j < ARG_MAX - 1) out[j++] = line[i++];
    out[j] = 0;
}

/* 把一行按空格拆成 tokens（就地改写，空格变 \0），返回 token 数 */
static int tokenize(char *line, char *tok[], uint32_t max) {
    uint32_t n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ') p++;
        if (!*p || n >= max) break;
        tok[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = 0; p++; }
    }
    return (int)n;
}

static void cmd_help(void) {
    sys_print("mini-os shell commands:\n");
    sys_print("  help            show this help\n");
    sys_print("  ls [path]       list directory (default /)\n");
    sys_print("  cat <path>      print file content\n");
    sys_print("  mkdir <path>    create directory\n");
    sys_print("  rmdir <path>    remove empty directory\n");
    sys_print("  rm <path>       delete file\n");
    sys_print("  run <prog>      load and run ELF app (hello/echo/crash/isol/forkdemo)\n");
    sys_print("  exec <prog> [a] fork + exec app with argv (forkdemo/args)\n");
    sys_print("  exit            quit shell\n");
}

static void cmd_ls(char *path) {
    syscall3(SYS_FS_LS, (uint32_t)path, 0, 0);   /* 内核按路径打印目录列表 */
}

static void cmd_mkdir(char *path) {
    if (!path[0]) { sys_print("usage: mkdir <path>\n"); return; }
    int rc = (int)syscall3(SYS_FS_MKDIR, (uint32_t)path, 0, 0);
    sys_print("[shell] mkdir '");
    sys_print(path);
    sys_print("' -> ");
    user_putdec((uint32_t)rc);
    sys_print("\n");
}

static void cmd_rmdir(char *path) {
    if (!path[0]) { sys_print("usage: rmdir <path>\n"); return; }
    int rc = (int)syscall3(SYS_FS_RMDIR, (uint32_t)path, 0, 0);
    sys_print("[shell] rmdir '");
    sys_print(path);
    sys_print("' -> ");
    user_putdec((uint32_t)rc);
    sys_print("\n");
}

static void cmd_rm(char *path) {
    if (!path[0]) { sys_print("usage: rm <path>\n"); return; }
    int rc = (int)syscall3(SYS_FS_DELETE, (uint32_t)path, 0, 0);
    sys_print("[shell] rm '");
    sys_print(path);
    sys_print("' -> ");
    user_putdec((uint32_t)rc);
    sys_print("\n");
}

static void cmd_cat(char *name) {
    if (!name[0]) { sys_print("usage: cat <file>\n"); return; }
    if (syscall3(SYS_FS_OPEN, 1, (uint32_t)name, 0) != 0) {   /* 只读，槽 1 */
        sys_print("cat: cannot open '");
        sys_print(name);
        sys_print("'\n");
        return;
    }
    for (;;) {
        char buf[64];
        int n = (int)syscall3(SYS_FS_READ, 1, (uint32_t)buf, 64);
        if (n <= 0) break;
        uint32_t i;
        for (i = 0; i < (uint32_t)n; i++) {
            char ch = buf[i];
            if (ch == '\n') sys_print("\n");
            else if (ch >= 32 && ch < 127) { char s[2] = { ch, 0 }; sys_print(s); }
            else { sys_print("."); }
        }
    }
    syscall3(SYS_FS_CLOSE, 1, 0, 0);
    sys_print("\n");
}

static void cmd_run(char *name) {
    if (!name[0]) { sys_print("usage: run <prog>\n"); return; }
    int pid = sys_spawn_file(name);
    if (pid <= 0) {
        sys_print("run: cannot load '");
        sys_print(name);
        sys_print("'\n");
        return;
    }
    sys_print("[shell] spawned '");
    sys_print(name);
    sys_print("' pid=");
    user_putdec((uint32_t)pid);
    sys_print("; waiting...\n");
    int code = 0;
    (void)sys_wait((uint32_t)pid, &code);   /* v0.15: 返回 pid + 退出码出参（这里只取 code） */
    sys_print("[shell] '");
    sys_print(name);
    sys_print("' exited code=");
    user_putdec((uint32_t)code);
    sys_print("\n");
}

/* v0.12: exec <prog> [args...] —— 经典 fork+exec+argv+wait 全链路。
 * shell fork 出子进程，子进程用 sys_exec 把自己替换为 prog 并携带 argv，
 * 父进程（shell）用 sys_wait 等待其退出。 */
static void cmd_exec(char *args) {
    char *tok[16];
    int n = tokenize(args, tok, 16);
    if (n < 1) { sys_print("usage: exec <prog> [args...]\n"); return; }
    char *av[10];
    uint32_t argc = 0;
    av[argc++] = tok[0];                          /* argv[0] = 程序名 */
    for (int i = 1; i < n && argc < 9; i++) av[argc++] = tok[i];
    av[argc] = 0;

    uint32_t pid = sys_fork();
    if (pid == 0) {
        /* 子进程：镜像替换为 prog（经典 fork+exec）；成功不返回 */
        (void)sys_exec(tok[0], argc, (const char **)av);
        sys_print("[exec] FAILED to exec '");
        sys_print(tok[0]);
        sys_print("'\n");
        sys_exit(1);
    }
    /* 父进程（shell）：等子进程退出 */
    sys_print("[shell] fork child pid=");
    user_putdec(pid);
    sys_print(", exec '");
    sys_print(tok[0]);
    sys_print("'\n");
    int code = 0;
    (void)sys_wait(pid, &code);   /* v0.15: 返回 pid + 退出码出参 */
    sys_print("[shell] '");
    sys_print(tok[0]);
    sys_print("' exited code=");
    user_putdec((uint32_t)code);
    sys_print("\n");
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[CMD_MAX];
    char cmd[ARG_MAX];
    char arg[ARG_MAX];

    sys_print("\n=== Mini-OS v0.15 shell ===\n");
    sys_print("type 'help' for commands\n");

    for (;;) {
        sys_print("mini-os$ ");
        int n = sys_readline(line, CMD_MAX);
        if (n < 0) continue;                    /* 无行可用（正常不应发生） */
        if (n == 0) continue;                   /* 空行：重新提示 */

        if (!split_cmd(line, cmd)) continue;
        split_arg(line, arg);

        if (user_strcmp(cmd, "help") == 0)      cmd_help();
        else if (user_strcmp(cmd, "ls") == 0)   cmd_ls(arg);
        else if (user_strcmp(cmd, "cat") == 0)  cmd_cat(arg);
        else if (user_strcmp(cmd, "mkdir") == 0) cmd_mkdir(arg);
        else if (user_strcmp(cmd, "rmdir") == 0) cmd_rmdir(arg);
        else if (user_strcmp(cmd, "rm") == 0)   cmd_rm(arg);
        else if (user_strcmp(cmd, "run") == 0)  cmd_run(arg);
        else if (user_strcmp(cmd, "exec") == 0) cmd_exec(arg);
        else if (user_strcmp(cmd, "exit") == 0) {
            sys_print("bye\n");
            sys_exit(0);
        } else {
            sys_print("unknown command: ");
            sys_print(cmd);
            sys_print("  (try 'help')\n");
        }
    }
}
