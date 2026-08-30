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
    sys_print("  save            write FS back to disk (v0.16 persist)\n");
    sys_print("  netping [ip][p] UDP ping to host echo (v0.22, default 10.0.2.2:7777)\n");
    sys_print("  ccboot          self-host bootstrap: cc500 compiles itself twice (v0.27)\n");
    sys_print("  selftest        run all demos, print one-line PASS/FAIL (agent-verifiable)\n");
    sys_print("  exit            quit shell\n");
}

static void cmd_save(void) {
    int rc = sys_fs_sync();
    sys_print("[shell] save -> ");
    user_putdec((uint32_t)rc);
    sys_print(rc == 0 ? " (fs saved to disk)\n" : " (no disk, memory only)\n");
}

/* 跑一个应用并等其退出，返回退出码 */
static int selftest_one(const char *name) {
    int pid = sys_spawn_file(name);
    if (pid <= 0) return -1;
    int code = 0;
    (void)sys_wait((uint32_t)pid, &code);
    return code;
}

/* v0.16 单行结构化自检：逐跑代表性演示（覆盖 spawn/隔离/fork/FS/wait），
 * 每项打印退出码，最后汇总为一行 `[selftest] PASS (N checks)` / FAIL，供 agent grep。
 * v0.21：追加第 6 项——内核自审计（帧配平/信号量守恒/PCB 状态机），
 * 使 [selftest] PASS 从"5 个应用没崩"升级为"内核核心不变量成立"。 */
static void cmd_selftest(void) {
    static const char *apps[] = { "hello", "isol", "forkdemo", "fsdemo", "waitdemo" };
    uint32_t n = sizeof(apps) / sizeof(apps[0]);
    uint32_t pass = 0, fail = 0;
    for (uint32_t i = 0; i < n; i++) {
        int code = selftest_one(apps[i]);
        sys_print("[selftest] '");
        sys_print(apps[i]);
        sys_print("' code=");
        user_putdec((uint32_t)code);
        sys_print("\n");
        if (code == 0) pass++; else fail++;
    }
    /* v0.21: 内核自审计作为第 6 项检查；内核打印 [audit] 详情，这里只汇总失败数 */
    uint32_t audit = sys_kern_audit();
    sys_print("[selftest] audit=");
    user_putdec(audit);
    sys_print("\n");
    if (audit == 0) pass++; else fail++;
    if (fail == 0) {
        sys_print("[selftest] PASS (");
        user_putdec(pass);
        sys_print(" checks)\n");
    } else {
        sys_print("[selftest] FAIL: ");
        user_putdec(fail);
        sys_print("/");
        user_putdec(n + 1);
        sys_print(" checks\n");
    }
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

/* ---- v0.22: netping —— 交互式网络连通性验证 ----
 * 用法：netping [ip] [port]（默认 10.0.2.2:7777 = SLIRP 网关/宿主 UDP echo）。
 * 开一个 UDP socket 发 PING，轮询收 PONG，单行原子打印
 * `[netping] <ip>:<port> PONG +<N>B rtt=<T> ticks` / `... FAIL`，供 agent grep。
 * 复用 sys_net_* 系统调用（与 sockdemo 同一链路），把"演示程序"变成"shell 交互命令"。 */

static uint32_t parse_ipv4(const char *s) {
    uint32_t ip = 0, oct = 0, cnt = 0;
    for (;;) {
        char c = *s++;
        if (c >= '0' && c <= '9') {
            oct = oct * 10 + (uint32_t)(c - '0');
            if (oct > 255) return 0;
        } else if (c == '.' || c == 0) {
            ip = (ip << 8) | oct;
            cnt++;
            oct = 0;
            if (c == 0) break;
        } else {
            return 0;
        }
    }
    return (cnt == 4) ? ip : 0;
}

static int parse_dec(const char *s) {
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
    }
    return v;
}

/* 单行原子打印助手（多进程并发写串口，拆多次 sys_print 会被内核日志插入） */
static char nl_buf[96];
static uint32_t nl_len;
static void nl_reset(void) { nl_len = 0; }
static void nl_s(const char *s) { while (*s && nl_len < sizeof(nl_buf) - 1) nl_buf[nl_len++] = *s++; }
static void nl_u(uint32_t v) {
    char t[12]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) nl_buf[nl_len++] = t[--i];
}
static void nl_end(void) { nl_buf[nl_len++] = '\n'; nl_buf[nl_len] = 0; sys_print(nl_buf); }

static void cmd_netping(char *args) {
    char *tok[4];
    int n = tokenize(args, tok, 4);
    uint32_t ip = 0x0A000202u;          /* 默认 10.0.2.2：SLIRP 网关 */
    uint16_t port = 7777;               /* 默认宿主 UDP echo 端口 */
    if (n >= 1) {
        uint32_t p = parse_ipv4(tok[0]);
        if (!p) { sys_print("usage: netping [ip] [port]\n"); return; }
        ip = p;
    }
    if (n >= 2) {
        int v = parse_dec(tok[1]);
        if (v <= 0) { sys_print("usage: netping [ip] [port]\n"); return; }
        port = (uint16_t)v;
    }

    int s = sys_net_socket(0);
    if (s < 0) { sys_print("[netping] socket() FAIL\n"); return; }
    nl_reset();
    nl_s("[netping] "); nl_u((ip >> 24) & 0xFF); nl_s(".");
    nl_u((ip >> 16) & 0xFF); nl_s("."); nl_u((ip >> 8) & 0xFF); nl_s(".");
    nl_u(ip & 0xFF); nl_s(":"); nl_u(port); nl_s(" ");

    uint32_t t0 = sys_getticks();
    int ok = 0, got = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        struct net_send_iov si;
        si.dst_ip = ip; si.dst_port = port;
        si.buf = (const uint8_t *)"PING"; si.len = 4;
        if (sys_net_sendto(s, &si) != 4) break;
        for (int i = 0; i < 200 && !ok; i++) {
            uint8_t rxb[64];
            struct net_recv_iov ri;
            ri.buf = rxb; ri.max = sizeof(rxb); ri.src_ip = 0; ri.src_port = 0;
            int m = sys_net_recvfrom(s, &ri);
            if (m >= 4 && rxb[0] == 'P' && rxb[1] == 'O' && rxb[2] == 'N' && rxb[3] == 'G') {
                got = m - 4; ok = 1;
                break;
            }
            sys_sleep(1);               /* 1 tick 后再试，让包有时间回来 */
        }
    }
    uint32_t rtt = sys_getticks() - t0;
    if (ok) {
        nl_s("PONG +"); nl_u((uint32_t)got); nl_s("B rtt="); nl_u(rtt); nl_s(" ticks");
    } else {
        nl_s("FAIL (no PONG in "); nl_u(rtt); nl_s(" ticks)");
    }
    nl_end();
    sys_net_close(s);
}

/* v0.27: 自举闭环验证（写-编-跑）。
 * 步骤：run cc500（gcc 版）编译 /cc500.c -> /out.elf=P1；再 run /out.elf（=P1）
 * 编译 /cc500.c -> /out.elf=P2；校验 P1 与 P2 的 FNV-1a 校验和与字节数一致。
 * 一致 => 编译器对自身源码是"不动点"，自举成立。单行原子输出供回归 grep。 */
static uint32_t file_fnv(const char *path, uint32_t *size_out) {
    uint32_t h = 0x811C9DC5u;
    uint32_t sz = 0;
    if (syscall3(SYS_FS_OPEN, 1, (uint32_t)path, 0) != 0) { *size_out = 0; return 0; }
    for (;;) {
        char buf[64];
        int n = (int)syscall3(SYS_FS_READ, 1, (uint32_t)buf, 64);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) { h ^= (uint8_t)buf[i]; h *= 0x01000193u; }
        sz += (uint32_t)n;
    }
    syscall3(SYS_FS_CLOSE, 1, 0, 0);
    *size_out = sz;
    return h;
}

static void cmd_ccboot(void) {
    /* 第 1 步：gcc 版编译器编译 /cc500.c -> /out.elf = P1 */
    int pid = sys_spawn_file("cc500");
    if (pid <= 0) { sys_print("[ccboot] cannot spawn cc500\n"); return; }
    int code = 0;
    (void)sys_wait((uint32_t)pid, &code);
    uint32_t s1, n1;
    s1 = file_fnv("/out.elf", &n1);
    /* 第 2 步：P1 再编译 /cc500.c -> /out.elf = P2 */
    pid = sys_spawn_file("/out.elf");
    if (pid <= 0) { sys_print("[ccboot] cannot spawn /out.elf (P1)\n"); return; }
    code = 0;
    (void)sys_wait((uint32_t)pid, &code);
    uint32_t s2, n2;
    s2 = file_fnv("/out.elf", &n2);
    /* 汇总单行：[ccboot] sha1=.. sha2=.. bytes=.. PASS/FAIL */
    nl_reset();
    nl_s("[ccboot] sha1="); nl_u(s1); nl_s(" sha2="); nl_u(s2);
    nl_s(" bytes="); nl_u(n1);
    nl_s((n1 > 0 && n1 == n2 && s1 == s2) ? " PASS\n" : " FAIL\n");
    nl_end();
}

void app_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[CMD_MAX];
    char cmd[ARG_MAX];
    char arg[ARG_MAX];

    sys_print("\n=== Mini-OS v0.27 shell ===\n");
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
        else if (user_strcmp(cmd, "save") == 0) cmd_save();
        else if (user_strcmp(cmd, "netping") == 0) cmd_netping(arg);
        else if (user_strcmp(cmd, "ccboot") == 0)  cmd_ccboot();
        else if (user_strcmp(cmd, "selftest") == 0) cmd_selftest();
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
