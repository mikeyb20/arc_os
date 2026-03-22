/* arc_os — Interactive shell v0.2
 * Built-in commands using direct syscalls. Freestanding, no libc.
 * Features: PATH lookup, quoting, shell variables, variable expansion. */

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
#define SYS_DUP2    14
#define SYS_GETPPID 15
#define SYS_FORK    16
#define SYS_EXEC    17
#define SYS_WAIT    18
#define SYS_PIPE    19
#define SYS_CHDIR   23
#define SYS_GETCWD  24
#define SYS_GETUID  25
#define SYS_GETGID  26
#define SYS_SETUID  27
#define SYS_SETGID  28
#define SYS_CHMOD   29
#define SYS_CHOWN   30

/* --- Open flags (must match kernel/fs/vfs.h) --- */

#define O_RDONLY  0x00
#define O_WRONLY  0x01
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

/* --- VFS types (must match kernel/fs/vfs.h) --- */

#define VFS_FILE      0
#define VFS_DIRECTORY 1

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
    uint32_t uid;
    uint32_t gid;
} StatInfo;

/* --- Parsing constants --- */

#define MAX_ARGS 16
#define MAX_ARGV (MAX_ARGS + 1)  /* +1 for NULL terminator */
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

static uint64_t sh_strlcpy(char *dst, const char *src, uint64_t size) {
    uint64_t slen = sh_strlen(src);
    if (size > 0) {
        uint64_t copy = slen < size - 1 ? slen : size - 1;
        for (uint64_t i = 0; i < copy; i++) dst[i] = src[i];
        dst[copy] = '\0';
    }
    return slen;
}

static char *sh_strchr(const char *s, int c) {
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return (void *)0;
}

static int sh_strncmp(const char *a, const char *b, uint64_t n) __attribute__((unused));
static int sh_strncmp(const char *a, const char *b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

static void sh_memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
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

static void print_octal(uint32_t n) {
    char buf[12];
    int pos = 11;
    buf[pos] = '\0';
    if (n == 0) { buf[--pos] = '0'; }
    while (n > 0) {
        buf[--pos] = '0' + (char)(n & 7);
        n >>= 3;
    }
    print(&buf[pos]);
}

static void mode_to_rwx(uint32_t mode, char *buf) {
    buf[0] = (mode & 0400) ? 'r' : '-';
    buf[1] = (mode & 0200) ? 'w' : '-';
    buf[2] = (mode & 0100) ? 'x' : '-';
    buf[3] = (mode & 0040) ? 'r' : '-';
    buf[4] = (mode & 0020) ? 'w' : '-';
    buf[5] = (mode & 0010) ? 'x' : '-';
    buf[6] = (mode & 0004) ? 'r' : '-';
    buf[7] = (mode & 0002) ? 'w' : '-';
    buf[8] = (mode & 0001) ? 'x' : '-';
    buf[9] = '\0';
}

static uint32_t sh_parse_octal(const char *s) {
    uint32_t val = 0;
    while (*s >= '0' && *s <= '7') {
        val = val * 8 + (uint32_t)(*s - '0');
        s++;
    }
    return val;
}

static void print_error(const char *prefix, int64_t err) {
    print(prefix);
    print(": error ");
    print_num(-err);
    print("\n");
}

/* --- cwd tracking --- */

static char shell_cwd[512];

static void refresh_cwd(void) {
    syscall3(SYS_GETCWD, (uint64_t)shell_cwd, 512, 0);
}

/* --- Shell variables --- */

#define MAX_VARS      32
#define VAR_NAME_MAX  32
#define VAR_VALUE_MAX 128

static char var_names[MAX_VARS][VAR_NAME_MAX];
static char var_values[MAX_VARS][VAR_VALUE_MAX];
static int  var_count = 0;

static int shell_findvar(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (sh_strcmp(var_names[i], name) == 0) return i;
    return -1;
}

static const char *shell_getvar(const char *name) {
    int i = shell_findvar(name);
    return i >= 0 ? var_values[i] : (void *)0;
}

static int shell_setvar(const char *name, const char *value) {
    if (sh_strlen(name) >= VAR_NAME_MAX || sh_strlen(value) >= VAR_VALUE_MAX)
        return -1;
    int i = shell_findvar(name);
    if (i >= 0) {
        sh_strlcpy(var_values[i], value, VAR_VALUE_MAX);
        return 0;
    }
    if (var_count >= MAX_VARS) return -1;
    sh_strlcpy(var_names[var_count], name, VAR_NAME_MAX);
    sh_strlcpy(var_values[var_count], value, VAR_VALUE_MAX);
    var_count++;
    return 0;
}

static int shell_unsetvar(const char *name) {
    int i = shell_findvar(name);
    if (i < 0) return -1;
    var_count--;
    if (i < var_count) {
        sh_memcpy(var_names[i], var_names[var_count], VAR_NAME_MAX);
        sh_memcpy(var_values[i], var_values[var_count], VAR_VALUE_MAX);
    }
    return 0;
}

/* --- Variable expansion --- */

static int is_var_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_var_char(char c) {
    return is_var_start(c) || (c >= '0' && c <= '9');
}

/* Expand $NAME references in input, write result to output.
 * Quotes are passed through for parse_line to strip.
 * Returns 0 on success, -1 if output overflows. */
static int expand_variables(const char *input, char *output, uint64_t output_size) {
    uint64_t wi = 0;
    int in_single = 0, in_double = 0;

    for (uint64_t ri = 0; input[ri]; ri++) {
        char ch = input[ri];

        /* Single quote toggle (not inside double quotes) */
        if (ch == '\'' && !in_double) {
            in_single = !in_single;
            if (wi >= output_size - 1) return -1;
            output[wi++] = ch;
            continue;
        }

        /* Double quote toggle (not inside single quotes) */
        if (ch == '"' && !in_single) {
            in_double = !in_double;
            if (wi >= output_size - 1) return -1;
            output[wi++] = ch;
            continue;
        }

        /* Inside single quotes — no expansion, pass through verbatim */
        if (in_single) {
            if (wi >= output_size - 1) return -1;
            output[wi++] = ch;
            continue;
        }

        /* Backslash handling (outside single quotes) */
        if (ch == '\\') {
            if (in_double) {
                /* Inside double quotes: only escape $, ", \ */
                char next = input[ri + 1];
                if (next == '$' || next == '"' || next == '\\') {
                    if (wi >= output_size - 1) return -1;
                    output[wi++] = next;
                    ri++;
                    continue;
                }
            } else {
                /* Outside quotes: \$ → literal $ */
                if (input[ri + 1] == '$') {
                    if (wi >= output_size - 1) return -1;
                    output[wi++] = '$';
                    ri++;
                    continue;
                }
            }
            /* Other backslash: pass through */
            if (wi >= output_size - 1) return -1;
            output[wi++] = ch;
            continue;
        }

        /* Dollar expansion */
        if (ch == '$') {
            /* $$ → literal $ */
            if (input[ri + 1] == '$') {
                if (wi >= output_size - 1) return -1;
                output[wi++] = '$';
                ri++;
                continue;
            }
            /* $NAME */
            if (is_var_start(input[ri + 1])) {
                uint64_t start = ri + 1;
                uint64_t end = start;
                while (is_var_char(input[end])) end++;

                char name[VAR_NAME_MAX];
                uint64_t nlen = end - start;
                if (nlen >= VAR_NAME_MAX) nlen = VAR_NAME_MAX - 1;
                for (uint64_t j = 0; j < nlen; j++) name[j] = input[start + j];
                name[nlen] = '\0';

                const char *val = shell_getvar(name);
                if (val) {
                    uint64_t vlen = sh_strlen(val);
                    if (wi + vlen >= output_size) return -1;
                    for (uint64_t j = 0; j < vlen; j++) output[wi++] = val[j];
                }
                ri = end - 1; /* for loop will ri++ */
                continue;
            }
            /* Bare $ at end or before non-name char → literal */
            if (wi >= output_size - 1) return -1;
            output[wi++] = '$';
            continue;
        }

        /* Normal character */
        if (wi >= output_size - 1) return -1;
        output[wi++] = ch;
    }

    output[wi] = '\0';
    return 0;
}

/* --- Line parsing (quote-aware) --- */

static int parse_line(char *line, char *argv[MAX_ARGV]) {
    /* Strip trailing newline */
    for (int i = 0; line[i]; i++) {
        if (line[i] == '\n') { line[i] = '\0'; break; }
    }

    int argc = 0;
    int in_token = 0;
    int wi = 0; /* write index (always <= ri) */

    for (int ri = 0; line[ri]; ri++) {
        char ch = line[ri];

        if (!in_token) {
            if (ch == ' ') continue;
            /* Start new token */
            if (argc >= MAX_ARGS) break;
            argv[argc++] = &line[wi];
            in_token = 1;
        }

        /* Inside a token */
        if (ch == ' ') {
            line[wi++] = '\0';
            in_token = 0;
            continue;
        }

        if (ch == '\'') {
            /* Single quote — copy verbatim until closing quote */
            ri++;
            while (line[ri] && line[ri] != '\'') {
                line[wi++] = line[ri++];
            }
            if (!line[ri]) ri--; /* compensate for loop ri++ */
            continue;
        }

        if (ch == '"') {
            /* Double quote — copy until closing, backslash escapes ", \, $ */
            ri++;
            while (line[ri] && line[ri] != '"') {
                if (line[ri] == '\\' && line[ri + 1]) {
                    char next = line[ri + 1];
                    if (next == '"' || next == '\\' || next == '$') {
                        line[wi++] = next;
                        ri += 2;
                        continue;
                    }
                }
                line[wi++] = line[ri++];
            }
            if (!line[ri]) ri--; /* compensate for loop ri++ */
            continue;
        }

        if (ch == '\\' && line[ri + 1]) {
            /* Backslash escape — emit next char literally */
            ri++;
            line[wi++] = line[ri];
            continue;
        }

        /* Normal character */
        line[wi++] = ch;
    }

    if (in_token) {
        line[wi] = '\0';
    }

    argv[argc] = (void *)0; /* NULL-terminate for exec */
    return argc;
}

/* Forward declaration for dispatch (needed by execute_with_redirect) */
static void dispatch(int argc, char *argv[]);

/* --- I/O Redirection --- */

typedef struct {
    const char *in_file;   /* < filename (NULL if none) */
    const char *out_file;  /* > or >> filename (NULL if none) */
    int         append;    /* 1 if >>, 0 if > */
} Redirect;

/* Scan argv for >, >>, < tokens. Compact argv in-place, decrement argc.
 * Returns 0 on success, -1 if an operator has no filename after it. */
static int parse_redirects(int *argc, char *argv[], Redirect *r) {
    r->in_file = (void *)0;
    r->out_file = (void *)0;
    r->append = 0;

    int out = 0;
    int i = 0;
    while (i < *argc) {
        char *tok = argv[i];

        /* Check >> first (before >) */
        if (tok[0] == '>' && tok[1] == '>') {
            /* >>file (attached) or >> file (separated) */
            if (tok[2] != '\0') {
                r->out_file = &tok[2];
            } else {
                i++;
                if (i >= *argc) {
                    print("syntax error: missing filename after >>\n");
                    return -1;
                }
                r->out_file = argv[i];
            }
            r->append = 1;
            i++;
        } else if (tok[0] == '>') {
            /* >file (attached) or > file (separated) */
            if (tok[1] != '\0') {
                r->out_file = &tok[1];
            } else {
                i++;
                if (i >= *argc) {
                    print("syntax error: missing filename after >\n");
                    return -1;
                }
                r->out_file = argv[i];
            }
            r->append = 0;
            i++;
        } else if (tok[0] == '<') {
            /* <file (attached) or < file (separated) */
            if (tok[1] != '\0') {
                r->in_file = &tok[1];
            } else {
                i++;
                if (i >= *argc) {
                    print("syntax error: missing filename after <\n");
                    return -1;
                }
                r->in_file = argv[i];
            }
            i++;
        } else {
            argv[out++] = argv[i++];
        }
    }
    *argc = out;
    return 0;
}

/* Open files and dup2 to stdin/stdout. Returns 0 on success, -1 on error. */
static int apply_redirects(const Redirect *r) {
    if (r->in_file) {
        int64_t fd = syscall3(SYS_OPEN, (uint64_t)r->in_file, O_RDONLY, 0);
        if (fd < 0) { print_error(r->in_file, fd); return -1; }
        syscall3(SYS_DUP2, (uint64_t)fd, 0, 0);
        syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
    }
    if (r->out_file) {
        int flags = O_WRONLY | O_CREAT;
        flags |= r->append ? O_APPEND : O_TRUNC;
        int64_t fd = syscall3(SYS_OPEN, (uint64_t)r->out_file, (uint64_t)flags, 0);
        if (fd < 0) { print_error(r->out_file, fd); return -1; }
        syscall3(SYS_DUP2, (uint64_t)fd, 1, 0);
        syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
    }
    return 0;
}

/* --- PATH resolution --- */

/* Search PATH for cmd; write full path to resolved. Returns 1 if found, 0 if not. */
static int resolve_command(const char *cmd, char *resolved, uint64_t buf_size) {
    /* If cmd contains '/', use as-is */
    if (sh_strchr(cmd, '/')) {
        sh_strlcpy(resolved, cmd, buf_size);
        return 1;
    }

    const char *path = shell_getvar("PATH");
    if (!path) return 0;

    /* Walk PATH entries separated by ':' */
    while (*path) {
        const char *sep = sh_strchr(path, ':');
        uint64_t dlen = sep ? (uint64_t)(sep - path) : sh_strlen(path);
        uint64_t clen = sh_strlen(cmd);

        if (dlen + 1 + clen < buf_size) {
            for (uint64_t i = 0; i < dlen; i++) resolved[i] = path[i];
            resolved[dlen] = '/';
            for (uint64_t i = 0; i < clen; i++) resolved[dlen + 1 + i] = cmd[i];
            resolved[dlen + 1 + clen] = '\0';

            StatInfo st;
            sh_memset(&st, 0, sizeof(st));
            int64_t r = syscall3(SYS_STAT, (uint64_t)resolved, (uint64_t)&st, 0);
            if (r == 0 && st.type == VFS_FILE) return 1;
        }

        if (!sep) break;
        path = sep + 1;
    }

    return 0;
}

/* Try resolve_command, then exec. Falls back to raw argv[0] if not found. */
static int64_t exec_resolved(char *argv[]) {
    char resolved[256];
    if (resolve_command(argv[0], resolved, 256))
        return syscall3(SYS_EXEC, (uint64_t)resolved, (uint64_t)argv, 0);
    return syscall3(SYS_EXEC, (uint64_t)argv[0], (uint64_t)argv, 0);
}

/* Fork, apply redirects in child, dispatch or exec, parent waits. */
static void execute_with_redirect(int argc, char *argv[], const Redirect *r) {
    int64_t pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        print_error("fork", pid);
        return;
    }
    if (pid == 0) {
        /* Child: apply redirects */
        if (apply_redirects(r) < 0) {
            syscall3(SYS_EXIT, 1, 0, 0);
            for (;;) __asm__ volatile ("ud2");
        }
        if (argc > 0) {
            /* Try exec first (pass argv for argument passing) */
            int64_t er = exec_resolved(argv);
            (void)er;
            /* Exec failed — try as builtin */
            dispatch(argc, argv);
        }
        syscall3(SYS_EXIT, 0, 0, 0);
        for (;;) __asm__ volatile ("ud2");
    }
    /* Parent: wait for child */
    syscall3(SYS_WAIT, 0, 0, 0);
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
static void cmd_cd(int argc, char *argv[]);
static void cmd_pwd(int argc, char *argv[]);
static void cmd_ppid(int argc, char *argv[]);
static void cmd_export(int argc, char *argv[]);
static void cmd_env(int argc, char *argv[]);
static void cmd_unset(int argc, char *argv[]);
static void cmd_whoami(int argc, char *argv[]);
static void cmd_id(int argc, char *argv[]);
static void cmd_chmod(int argc, char *argv[]);
static void cmd_chown(int argc, char *argv[]);

/* --- Dispatch table --- */

typedef void (*builtin_fn)(int argc, char *argv[]);

typedef struct {
    const char *name;
    builtin_fn  fn;
    const char *help;
} Builtin;

static const Builtin builtins[] = {
    { "help",   cmd_help,   "List available commands" },
    { "echo",   cmd_echo,   "Print arguments" },
    { "ls",     cmd_ls,     "List directory [path]" },
    { "cat",    cmd_cat,    "Display file contents" },
    { "stat",   cmd_stat,   "Show file metadata" },
    { "mkdir",  cmd_mkdir,  "Create directory" },
    { "rm",     cmd_rm,     "Delete file" },
    { "touch",  cmd_touch,  "Create empty file" },
    { "write",  cmd_write,  "Write text to file" },
    { "uname",  cmd_uname,  "Print OS name" },
    { "clear",  cmd_clear,  "Clear screen" },
    { "exit",   cmd_exit,   "Exit shell [code]" },
    { "run",    cmd_run,    "Execute binary (fork/exec)" },
    { "pid",    cmd_pid,    "Print current PID" },
    { "cd",     cmd_cd,     "Change directory [path]" },
    { "pwd",    cmd_pwd,    "Print working directory" },
    { "ppid",   cmd_ppid,   "Print parent PID" },
    { "export", cmd_export, "Set variable (export NAME=VALUE)" },
    { "env",    cmd_env,    "List all variables" },
    { "unset",  cmd_unset,  "Remove variable(s)" },
    { "whoami", cmd_whoami, "Print current user" },
    { "id",     cmd_id,     "Print uid/gid" },
    { "chmod",  cmd_chmod,  "Change permissions (octal)" },
    { "chown",  cmd_chown,  "Change owner (uid[:gid])" },
};
#define NUM_BUILTINS (sizeof(builtins) / sizeof(builtins[0]))

/* Check if arg matches NAME=VALUE pattern */
static int try_assignment(const char *arg) {
    if (!is_var_start(arg[0])) return 0;
    int i = 1;
    while (is_var_char(arg[i])) i++;
    return arg[i] == '=';
}

static void dispatch(int argc, char *argv[]) {
    if (argc == 0) return;

    /* Bare assignment: NAME=VALUE */
    if (argc == 1 && try_assignment(argv[0])) {
        char name[VAR_NAME_MAX];
        const char *eq = sh_strchr(argv[0], '=');
        uint64_t nlen = (uint64_t)(eq - argv[0]);
        if (nlen >= VAR_NAME_MAX) { print("name too long\n"); return; }
        for (uint64_t j = 0; j < nlen; j++) name[j] = argv[0][j];
        name[nlen] = '\0';
        shell_setvar(name, eq + 1);
        return;
    }

    for (int i = 0; i < (int)NUM_BUILTINS; i++) {
        if (sh_strcmp(argv[0], builtins[i].name) == 0) {
            builtins[i].fn(argc, argv);
            return;
        }
    }
    /* Not a builtin — try fork + exec */
    int64_t pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) { print_error("fork", pid); return; }
    if (pid == 0) {
        int64_t er = exec_resolved(argv);
        (void)er;
        print(argv[0]);
        println(": command not found");
        syscall3(SYS_EXIT, 127, 0, 0);
        for (;;) __asm__ volatile ("ud2");
    }
    syscall3(SYS_WAIT, 0, 0, 0);
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
    const char *path = argc >= 2 ? argv[1] : ".";
    DirEntry entries[16];
    int64_t count = syscall3(SYS_READDIR, (uint64_t)path, (uint64_t)entries, 16);
    if (count < 0) {
        print_error(path, count);
        return;
    }
    for (int64_t i = 0; i < count; i++) {
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

        /* Type + permissions */
        print(entries[i].type == VFS_DIRECTORY ? "d" : "-");
        if (sr == 0) {
            char rwx[10];
            mode_to_rwx(st.mode, rwx);
            print(rwx);
        } else {
            print("---------");
        }
        print(" ");

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
    if (argc < 2) {
        /* No file argument — read from stdin (for pipe support) */
        char buf[256];
        for (;;) {
            int64_t n = syscall3(SYS_READ, 0, (uint64_t)buf, 256);
            if (n <= 0) break;
            syscall3(SYS_WRITE, 1, (uint64_t)buf, (uint64_t)n);
        }
        return;
    }
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
    print("  Mode: 0"); print_octal(st.mode); print("\n");
    char rwx[10];
    mode_to_rwx(st.mode, rwx);
    print(" Perms: "); println(rwx);
    print(" Owner: "); print_num((int64_t)st.uid); print("\n");
    print(" Group: "); print_num((int64_t)st.gid); print("\n");
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
    /* Resolve before fork so child doesn't need SYS_STAT */
    char resolved[256];
    const char *exec_path = argv[1];
    if (resolve_command(argv[1], resolved, 256))
        exec_path = resolved;
    int64_t pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        print_error("fork", pid);
        return;
    }
    if (pid == 0) {
        /* Child: exec the binary (argv+1 so binary gets argv[0]=path) */
        syscall3(SYS_EXEC, (uint64_t)exec_path, (uint64_t)(argv + 1), 0);
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

static void cmd_cd(int argc, char *argv[]) {
    const char *path = argc >= 2 ? argv[1] : "/";
    int64_t r = syscall3(SYS_CHDIR, (uint64_t)path, 0, 0);
    if (r < 0) print_error(path, r);
    else refresh_cwd();
}

static void cmd_pwd(int argc, char *argv[]) {
    (void)argc; (void)argv;
    refresh_cwd();
    println(shell_cwd);
}

static void cmd_ppid(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int64_t p = syscall3(SYS_GETPPID, 0, 0, 0);
    print("PPID: ");
    print_num(p);
    print("\n");
}

static void cmd_export(int argc, char *argv[]) {
    if (argc < 2) {
        /* List all variables */
        for (int i = 0; i < var_count; i++) {
            print(var_names[i]);
            print("=");
            println(var_values[i]);
        }
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (try_assignment(argv[i])) {
            char name[VAR_NAME_MAX];
            const char *eq = sh_strchr(argv[i], '=');
            uint64_t nlen = (uint64_t)(eq - argv[i]);
            if (nlen >= VAR_NAME_MAX) { print("name too long\n"); continue; }
            for (uint64_t j = 0; j < nlen; j++) name[j] = argv[i][j];
            name[nlen] = '\0';
            shell_setvar(name, eq + 1);
        }
        /* export NAME with no '=' — no-op */
    }
}

static void cmd_env(int argc, char *argv[]) {
    (void)argc; (void)argv;
    for (int i = 0; i < var_count; i++) {
        print(var_names[i]);
        print("=");
        println(var_values[i]);
    }
}

static void cmd_unset(int argc, char *argv[]) {
    if (argc < 2) { println("usage: unset NAME [...]"); return; }
    for (int i = 1; i < argc; i++) {
        shell_unsetvar(argv[i]);
    }
}

static void cmd_whoami(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int64_t uid = syscall3(SYS_GETUID, 0, 0, 0);
    if (uid == 0) println("root");
    else { print("user"); print_num(uid); print("\n"); }
}

static void cmd_id(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int64_t uid = syscall3(SYS_GETUID, 0, 0, 0);
    int64_t gid = syscall3(SYS_GETGID, 0, 0, 0);
    print("uid="); print_num(uid);
    print(" gid="); print_num(gid);
    print("\n");
}

static void cmd_chmod(int argc, char *argv[]) {
    if (argc < 3) { println("usage: chmod <mode> <path>"); return; }
    uint32_t mode = sh_parse_octal(argv[1]);
    int64_t r = syscall3(SYS_CHMOD, (uint64_t)argv[2], (uint64_t)mode, 0);
    if (r < 0) print_error(argv[2], r);
}

static void cmd_chown(int argc, char *argv[]) {
    if (argc < 3) { println("usage: chown <uid[:gid]> <path>"); return; }
    uint32_t uid = 0, gid = (uint32_t)-1;
    const char *s = argv[1];
    while (*s >= '0' && *s <= '9') { uid = uid * 10 + (uint32_t)(*s - '0'); s++; }
    if (*s == ':') {
        s++; gid = 0;
        while (*s >= '0' && *s <= '9') { gid = gid * 10 + (uint32_t)(*s - '0'); s++; }
    }
    int64_t r = syscall3(SYS_CHOWN, (uint64_t)argv[2], (uint64_t)uid, (uint64_t)gid);
    if (r < 0) print_error(argv[2], r);
}

/* --- Pipe support --- */

/* Find first unquoted '|' in line. Returns pointer to it, or NULL. */
static char *find_pipe(char *line) {
    int in_single = 0, in_double = 0;
    for (int i = 0; line[i]; i++) {
        char ch = line[i];
        if (ch == '\\' && line[i + 1] && !in_single) { i++; continue; }
        if (ch == '\'' && !in_double) { in_single = !in_single; continue; }
        if (ch == '"' && !in_single) { in_double = !in_double; continue; }
        if (ch == '|' && !in_single && !in_double) return &line[i];
    }
    return (void *)0;
}

/* Strip leading spaces from a string */
static char *skip_spaces(char *s) {
    while (*s == ' ') s++;
    return s;
}

/* Execute a pipeline: left | right */
static void execute_pipe(char *left, char *right) {
    left = skip_spaces(left);
    right = skip_spaces(right);

    /* Create pipe */
    int32_t pipefd[2];
    int64_t err = syscall3(SYS_PIPE, (uint64_t)pipefd, 0, 0);
    if (err < 0) {
        print_error("pipe", err);
        return;
    }

    /* Fork child 1 (writer — runs left command, stdout → pipe write end) */
    int64_t pid1 = syscall3(SYS_FORK, 0, 0, 0);
    if (pid1 < 0) {
        print_error("fork", pid1);
        syscall3(SYS_CLOSE, (uint64_t)pipefd[0], 0, 0);
        syscall3(SYS_CLOSE, (uint64_t)pipefd[1], 0, 0);
        return;
    }
    if (pid1 == 0) {
        /* Child 1: close read end, dup write end to stdout, close original */
        syscall3(SYS_CLOSE, (uint64_t)pipefd[0], 0, 0);
        syscall3(SYS_DUP2, (uint64_t)pipefd[1], 1, 0);
        syscall3(SYS_CLOSE, (uint64_t)pipefd[1], 0, 0);

        /* Parse and run left command (with optional redirects) */
        char *argv[MAX_ARGV];
        int argc = parse_line(left, argv);
        if (argc > 0) {
            Redirect redir = {0, 0, 0};
            if (parse_redirects(&argc, argv, &redir) == 0)
                apply_redirects(&redir);
            /* Try exec first (pass argv) */
            int64_t r = exec_resolved(argv);
            (void)r;
            /* Exec failed — try as builtin */
            dispatch(argc, argv);
        }
        syscall3(SYS_EXIT, 0, 0, 0);
        for (;;) __asm__ volatile ("ud2");
    }

    /* Fork child 2 (reader — runs right command, stdin → pipe read end) */
    int64_t pid2 = syscall3(SYS_FORK, 0, 0, 0);
    if (pid2 < 0) {
        print_error("fork", pid2);
        syscall3(SYS_CLOSE, (uint64_t)pipefd[0], 0, 0);
        syscall3(SYS_CLOSE, (uint64_t)pipefd[1], 0, 0);
        /* Wait for child1 */
        syscall3(SYS_WAIT, 0, 0, 0);
        return;
    }
    if (pid2 == 0) {
        /* Child 2: close write end, dup read end to stdin, close original */
        syscall3(SYS_CLOSE, (uint64_t)pipefd[1], 0, 0);
        syscall3(SYS_DUP2, (uint64_t)pipefd[0], 0, 0);
        syscall3(SYS_CLOSE, (uint64_t)pipefd[0], 0, 0);

        /* Parse and run right command (with optional redirects) */
        char *argv[MAX_ARGV];
        int argc = parse_line(right, argv);
        if (argc > 0) {
            Redirect redir = {0, 0, 0};
            if (parse_redirects(&argc, argv, &redir) == 0)
                apply_redirects(&redir);
            int64_t r = exec_resolved(argv);
            (void)r;
            dispatch(argc, argv);
        }
        syscall3(SYS_EXIT, 0, 0, 0);
        for (;;) __asm__ volatile ("ud2");
    }

    /* Parent: close both pipe ends, wait for both children */
    syscall3(SYS_CLOSE, (uint64_t)pipefd[0], 0, 0);
    syscall3(SYS_CLOSE, (uint64_t)pipefd[1], 0, 0);
    syscall3(SYS_WAIT, 0, 0, 0);
    syscall3(SYS_WAIT, 0, 0, 0);
}

/* --- Entry point --- */

void _start(uint64_t argc, char **argv) {
    (void)argc; (void)argv;

    /* Initialize default variables */
    shell_setvar("PATH", "/boot");
    shell_setvar("HOME", "/");

    println("arc_os shell v0.2");
    println("Type 'help' for available commands.");
    print("\n");

    char line[LINE_MAX];
    char expanded[LINE_MAX * 2];
    char *args[MAX_ARGV];
    refresh_cwd();

    for (;;) {
        print("[");
        print(shell_cwd);
        print("]$ ");
        int64_t n = syscall3(SYS_READ, 0, (uint64_t)line, LINE_MAX - 1);
        if (n <= 0) break;
        line[n] = '\0';

        /* Strip trailing newline */
        for (int i = 0; line[i]; i++) {
            if (line[i] == '\n') { line[i] = '\0'; break; }
        }

        /* Expand variables */
        if (expand_variables(line, expanded, sizeof(expanded)) < 0) {
            println("expansion error: line too long");
            continue;
        }

        /* Check for pipe (quote-aware) */
        char *pipe_pos = find_pipe(expanded);
        if (pipe_pos != (void *)0) {
            *pipe_pos = '\0';
            execute_pipe(expanded, pipe_pos + 1);
        } else {
            int ac = parse_line(expanded, args);
            Redirect redir = {0, 0, 0};
            int rr = parse_redirects(&ac, args, &redir);
            if (rr < 0) continue;  /* parse error, already printed */
            if (redir.in_file || redir.out_file) {
                execute_with_redirect(ac, args, &redir);
            } else {
                dispatch(ac, args);
            }
        }
    }

    syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) __asm__ volatile ("ud2");
}
