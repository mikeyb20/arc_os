/* arc_os — Interactive shell
 * Built-in commands using direct syscalls. Freestanding, no libc. */

#include <stdint.h>

/* --- Syscall wrapper --- */

static inline int64_t syscall3(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* --- Syscall numbers --- */

#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_GETPID  2
#define SYS_OPEN    3
#define SYS_READ    4
#define SYS_CLOSE   5
#define SYS_STAT    8
#define SYS_MKDIR   9
#define SYS_READDIR 10
#define SYS_UNLINK  11
#define SYS_FORK    16
#define SYS_EXEC    17
#define SYS_WAIT    18

/* --- Open flags (must match kernel/fs/vfs.h) --- */

#define O_RDONLY  0x00
#define O_WRONLY  0x01
#define O_CREAT   0x40
#define O_TRUNC   0x200

/* --- VFS types (must match kernel/fs/vfs.h) --- */

#define VFS_FILE      1
#define VFS_DIRECTORY 2

/* --- Userland struct declarations (must match kernel layout exactly) --- */

typedef struct {
    char     name[256];
    uint64_t inode_num;
    uint8_t  type;
} DirEntry;

typedef struct {
    uint64_t inode_num;
    uint8_t  type;
    uint64_t size;
    uint32_t mode;
} StatInfo;

/* --- Parsing constants --- */

#define MAX_ARGS 16
#define LINE_MAX 256

/* --- String utilities (no libc available) --- */

static uint64_t sh_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static int sh_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void sh_memset(void *dst, int c, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

static int sh_atoi(const char *s) {
    int neg = 0, val = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return neg ? -val : val;
}

/* --- I/O helpers --- */

static void print(const char *s) {
    syscall3(SYS_WRITE, 1, (uint64_t)s, sh_strlen(s));
}

static void println(const char *s) {
    print(s);
    syscall3(SYS_WRITE, 1, (uint64_t)"\n", 1);
}

static void print_num(int64_t n) {
    char buf[21];
    int pos = 20;
    int neg = 0;
    buf[pos] = '\0';

    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) { buf[--pos] = '0'; }
    while (n > 0) {
        buf[--pos] = '0' + (char)(n % 10);
        n /= 10;
    }
    if (neg) buf[--pos] = '-';
    print(&buf[pos]);
}

static void print_error(const char *prefix, int64_t err) {
    print(prefix);
    print(": error ");
    print_num(-err);
    print("\n");
}

/* --- Line parsing --- */

static int parse_line(char *line, char *argv[MAX_ARGS]) {
    /* Strip trailing newline */
    for (int i = 0; line[i]; i++) {
        if (line[i] == '\n') { line[i] = '\0'; break; }
    }

    int argc = 0;
    int in_token = 0;

    for (int i = 0; line[i]; i++) {
        if (line[i] == ' ') {
            line[i] = '\0';
            in_token = 0;
        } else if (!in_token) {
            if (argc >= MAX_ARGS) break;
            argv[argc++] = &line[i];
            in_token = 1;
        }
    }
    return argc;
}

/* --- Builtin command implementations --- */

/* Forward declarations for dispatch table */
static void cmd_help(int argc, char *argv[]);
static void cmd_echo(int argc, char *argv[]);
static void cmd_ls(int argc, char *argv[]);
static void cmd_cat(int argc, char *argv[]);
static void cmd_stat(int argc, char *argv[]);
static void cmd_mkdir(int argc, char *argv[]);
static void cmd_rm(int argc, char *argv[]);
static void cmd_touch(int argc, char *argv[]);
static void cmd_write(int argc, char *argv[]);
static void cmd_uname(int argc, char *argv[]);
static void cmd_clear(int argc, char *argv[]);
static void cmd_exit(int argc, char *argv[]);
static void cmd_run(int argc, char *argv[]);
static void cmd_pid(int argc, char *argv[]);

/* --- Dispatch table --- */

typedef void (*builtin_fn)(int argc, char *argv[]);

typedef struct {
    const char *name;
    builtin_fn  fn;
    const char *help;
} Builtin;

static const Builtin builtins[] = {
    { "help",  cmd_help,  "List available commands" },
    { "echo",  cmd_echo,  "Print arguments" },
    { "ls",    cmd_ls,    "List directory [path]" },
    { "cat",   cmd_cat,   "Display file contents" },
    { "stat",  cmd_stat,  "Show file metadata" },
    { "mkdir", cmd_mkdir, "Create directory" },
    { "rm",    cmd_rm,    "Delete file" },
    { "touch", cmd_touch, "Create empty file" },
    { "write", cmd_write, "Write text to file" },
    { "uname", cmd_uname, "Print OS name" },
    { "clear", cmd_clear, "Clear screen" },
    { "exit",  cmd_exit,  "Exit shell [code]" },
    { "run",   cmd_run,   "Execute binary (fork/exec)" },
    { "pid",   cmd_pid,   "Print current PID" },
};
#define NUM_BUILTINS (sizeof(builtins) / sizeof(builtins[0]))

static void dispatch(int argc, char *argv[]) {
    if (argc == 0) return;
    for (int i = 0; i < (int)NUM_BUILTINS; i++) {
        if (sh_strcmp(argv[0], builtins[i].name) == 0) {
            builtins[i].fn(argc, argv);
            return;
        }
    }
    print("unknown command: ");
    println(argv[0]);
}

/* --- Command implementations --- */

static void cmd_help(int argc, char *argv[]) {
    (void)argc; (void)argv;
    println("Available commands:");
    for (int i = 0; i < (int)NUM_BUILTINS; i++) {
        print("  ");
        print(builtins[i].name);
        /* Pad to 10 chars */
        int len = (int)sh_strlen(builtins[i].name);
        for (int j = len; j < 10; j++) print(" ");
        println(builtins[i].help);
    }
}

static void cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) print(" ");
        print(argv[i]);
    }
    print("\n");
}

static void cmd_ls(int argc, char *argv[]) {
    const char *path = argc >= 2 ? argv[1] : "/";
    DirEntry entries[16];
    int64_t count = syscall3(SYS_READDIR, (uint64_t)path, (uint64_t)entries, 16);
    if (count < 0) {
        print_error(path, count);
        return;
    }
    for (int64_t i = 0; i < count; i++) {
        /* Show type prefix */
        print(entries[i].type == VFS_DIRECTORY ? "d " : "- ");

        /* Build full path for stat: path + "/" + name */
        char fullpath[512];
        uint64_t plen = sh_strlen(path);
        uint64_t nlen = sh_strlen(entries[i].name);
        if (plen + 1 + nlen >= 512) continue;

        /* Copy path */
        for (uint64_t j = 0; j < plen; j++) fullpath[j] = path[j];
        /* Add separator if needed */
        uint64_t pos = plen;
        if (plen > 0 && path[plen - 1] != '/') fullpath[pos++] = '/';
        /* Copy name */
        for (uint64_t j = 0; j < nlen; j++) fullpath[pos + j] = entries[i].name[j];
        fullpath[pos + nlen] = '\0';

        StatInfo st;
        sh_memset(&st, 0, sizeof(st));
        int64_t sr = syscall3(SYS_STAT, (uint64_t)fullpath, (uint64_t)&st, 0);

        if (sr == 0 && entries[i].type == VFS_FILE) {
            /* Right-align size in 8 chars */
            char size_buf[9];
            sh_memset(size_buf, ' ', 8);
            size_buf[8] = '\0';
            int64_t sz = (int64_t)st.size;
            int p = 7;
            if (sz == 0) {
                size_buf[p] = '0';
            } else {
                while (sz > 0 && p >= 0) {
                    size_buf[p--] = '0' + (char)(sz % 10);
                    sz /= 10;
                }
            }
            print(size_buf);
            print(" ");
        } else {
            print("         ");
        }

        println(entries[i].name);
    }
}

static void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) { println("usage: cat <file>"); return; }
    int64_t fd = syscall3(SYS_OPEN, (uint64_t)argv[1], O_RDONLY, 0);
    if (fd < 0) { print_error(argv[1], fd); return; }

    char buf[256];
    for (;;) {
        int64_t n = syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, 256);
        if (n <= 0) break;
        syscall3(SYS_WRITE, 1, (uint64_t)buf, (uint64_t)n);
    }
    syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
}

static void cmd_stat(int argc, char *argv[]) {
    if (argc < 2) { println("usage: stat <path>"); return; }
    StatInfo st;
    sh_memset(&st, 0, sizeof(st));
    int64_t r = syscall3(SYS_STAT, (uint64_t)argv[1], (uint64_t)&st, 0);
    if (r < 0) { print_error(argv[1], r); return; }

    print("  File: "); println(argv[1]);
    print("  Type: "); println(st.type == VFS_DIRECTORY ? "directory" : "file");
    print(" Inode: "); print_num((int64_t)st.inode_num); print("\n");
    print("  Size: "); print_num((int64_t)st.size); print("\n");
    print("  Mode: 0"); print_num((int64_t)st.mode); print("\n");
}

static void cmd_mkdir(int argc, char *argv[]) {
    if (argc < 2) { println("usage: mkdir <dir>"); return; }
    int64_t r = syscall3(SYS_MKDIR, (uint64_t)argv[1], 0755, 0);
    if (r < 0) print_error(argv[1], r);
}

static void cmd_rm(int argc, char *argv[]) {
    if (argc < 2) { println("usage: rm <file>"); return; }
    int64_t r = syscall3(SYS_UNLINK, (uint64_t)argv[1], 0, 0);
    if (r < 0) print_error(argv[1], r);
}

static void cmd_touch(int argc, char *argv[]) {
    if (argc < 2) { println("usage: touch <file>"); return; }
    int64_t fd = syscall3(SYS_OPEN, (uint64_t)argv[1], O_CREAT | O_WRONLY, 0);
    if (fd < 0) { print_error(argv[1], fd); return; }
    syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
}

static void cmd_write(int argc, char *argv[]) {
    if (argc < 3) { println("usage: write <file> <text...>"); return; }
    int64_t fd = syscall3(SYS_OPEN, (uint64_t)argv[1], O_CREAT | O_WRONLY | O_TRUNC, 0);
    if (fd < 0) { print_error(argv[1], fd); return; }

    for (int i = 2; i < argc; i++) {
        if (i > 2) syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)" ", 1);
        syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)argv[i], sh_strlen(argv[i]));
    }
    syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)"\n", 1);
    syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
}

static void cmd_uname(int argc, char *argv[]) {
    (void)argc; (void)argv;
    println("arc_os");
}

static void cmd_clear(int argc, char *argv[]) {
    (void)argc; (void)argv;
    print("\033[2J\033[H");
}

static void cmd_exit(int argc, char *argv[]) {
    int code = 0;
    if (argc >= 2) code = sh_atoi(argv[1]);
    syscall3(SYS_EXIT, (uint64_t)code, 0, 0);
    for (;;) __asm__ volatile ("ud2");
}

static void cmd_run(int argc, char *argv[]) {
    if (argc < 2) { println("usage: run <path>"); return; }
    int64_t pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        print_error("fork", pid);
        return;
    }
    if (pid == 0) {
        /* Child: exec the binary */
        syscall3(SYS_EXEC, (uint64_t)argv[1], 0, 0);
        /* If exec fails, exit */
        println("exec failed");
        syscall3(SYS_EXIT, 1, 0, 0);
        for (;;) __asm__ volatile ("ud2");
    }
    /* Parent: wait for child */
    int32_t status = -1;
    syscall3(SYS_WAIT, (uint64_t)&status, 0, 0);
    print("exited with status ");
    print_num((int64_t)status);
    print("\n");
}

static void cmd_pid(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int64_t p = syscall3(SYS_GETPID, 0, 0, 0);
    print("PID: ");
    print_num(p);
    print("\n");
}

/* --- Entry point --- */

void _start(void) {
    println("arc_os shell v0.1");
    println("Type 'help' for available commands.");
    print("\n");

    char line[LINE_MAX];
    char *argv[MAX_ARGS];

    for (;;) {
        print("shell> ");
        int64_t n = syscall3(SYS_READ, 0, (uint64_t)line, LINE_MAX - 1);
        if (n <= 0) break;
        line[n] = '\0';

        int argc = parse_line(line, argv);
        dispatch(argc, argv);
    }

    syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) __asm__ volatile ("ud2");
}
