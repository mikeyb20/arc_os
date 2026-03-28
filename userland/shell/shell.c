/* arc_os — Interactive shell v0.2
 * Built-in commands using libarc. Linked against libarc (CRT0 provides _start).
 * Features: PATH lookup, quoting, shell variables, variable expansion. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <syscall.h>
#include <errno.h>

/* --- VFS types (must match kernel/fs/vfs.h) --- */

#define VFS_FILE      0
#define VFS_DIRECTORY 1

/* --- DirEntry for SYS_READDIR (must match kernel VfsDirEntry layout) --- */

typedef struct {
    char     name[256];
    uint64_t inode_num;
    uint8_t  type;
} DirEntry;

/* --- Parsing constants --- */

#define MAX_ARGS 16
#define MAX_ARGV (MAX_ARGS + 1)  /* +1 for NULL terminator */
#define LINE_MAX 256

/* --- Octal parsing helper --- */

static uint32_t sh_parse_octal(const char *s) {
    uint32_t val = 0;
    while (*s >= '0' && *s <= '7') {
        val = val * 8 + (uint32_t)(*s - '0');
        s++;
    }
    return val;
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

static void print_error(const char *prefix, int err) {
    printf("%s: error %d\n", prefix, err);
}

/* --- cwd tracking --- */

static char shell_cwd[512];

static void refresh_cwd(void) {
    getcwd(shell_cwd, 512);
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
        if (strcmp(var_names[i], name) == 0) return i;
    return -1;
}

static const char *shell_getvar(const char *name) {
    int i = shell_findvar(name);
    return i >= 0 ? var_values[i] : NULL;
}

static int shell_setvar(const char *name, const char *value) {
    if (strlen(name) >= VAR_NAME_MAX || strlen(value) >= VAR_VALUE_MAX)
        return -1;
    int i = shell_findvar(name);
    if (i >= 0) {
        strncpy(var_values[i], value, VAR_VALUE_MAX - 1);
        var_values[i][VAR_VALUE_MAX - 1] = '\0';
        return 0;
    }
    if (var_count >= MAX_VARS) return -1;
    strncpy(var_names[var_count], name, VAR_NAME_MAX - 1);
    var_names[var_count][VAR_NAME_MAX - 1] = '\0';
    strncpy(var_values[var_count], value, VAR_VALUE_MAX - 1);
    var_values[var_count][VAR_VALUE_MAX - 1] = '\0';
    var_count++;
    return 0;
}

static int shell_unsetvar(const char *name) {
    int i = shell_findvar(name);
    if (i < 0) return -1;
    var_count--;
    if (i < var_count) {
        memcpy(var_names[i], var_names[var_count], VAR_NAME_MAX);
        memcpy(var_values[i], var_values[var_count], VAR_VALUE_MAX);
    }
    return 0;
}

/* --- Job control --- */

#define MAX_JOBS 8

typedef struct {
    int      in_use;
    int      job_id;     /* 1-based job number */
    int32_t  pid;        /* Lead process PID */
    int32_t  pgid;       /* Process group ID */
    int      stopped;    /* 1 if stopped, 0 if running */
    char     cmd[128];   /* Command string for display */
} Job;

static Job job_table[MAX_JOBS];
static int next_job_id = 1;
static int32_t shell_pgid;

static Job *job_alloc(int32_t pid, int32_t pgid, const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!job_table[i].in_use) {
            job_table[i].in_use = 1;
            job_table[i].job_id = next_job_id++;
            job_table[i].pid = pid;
            job_table[i].pgid = pgid;
            job_table[i].stopped = 0;
            strncpy(job_table[i].cmd, cmd, 127);
            job_table[i].cmd[127] = '\0';
            return &job_table[i];
        }
    }
    return NULL;
}

static Job *job_find_by_id(int id) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].in_use && job_table[i].job_id == id)
            return &job_table[i];
    }
    return NULL;
}

static void job_free(Job *j) {
    j->in_use = 0;
}

static void job_print(Job *j) {
    printf("[%d] %s%s\n", j->job_id,
           j->stopped ? "Stopped    " : "Running    ",
           j->cmd);
}

/* Reap any completed background jobs (non-blocking) */
static void reap_background_jobs(void) {
    int status = 0;
    pid_t w;
    while ((w = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Find the job by PID */
        for (int i = 0; i < MAX_JOBS; i++) {
            if (job_table[i].in_use && job_table[i].pid == (int32_t)w) {
                if (WIFSTOPPED(status)) {
                    job_table[i].stopped = 1;
                } else {
                    printf("[%d] Done       %s\n",
                           job_table[i].job_id, job_table[i].cmd);
                    job_free(&job_table[i]);
                }
                break;
            }
        }
    }
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
                /* Outside quotes: \$ -> literal $ */
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
            /* $$ -> literal $ */
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
                    uint64_t vlen = strlen(val);
                    if (wi + vlen >= output_size) return -1;
                    for (uint64_t j = 0; j < vlen; j++) output[wi++] = val[j];
                }
                ri = end - 1; /* for loop will ri++ */
                continue;
            }
            /* Bare $ at end or before non-name char -> literal */
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

    argv[argc] = NULL; /* NULL-terminate for exec */
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
    r->in_file = NULL;
    r->out_file = NULL;
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
                    printf("syntax error: missing filename after >>\n");
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
                    printf("syntax error: missing filename after >\n");
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
                    printf("syntax error: missing filename after <\n");
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
        int fd = open(r->in_file, O_RDONLY);
        if (fd < 0) { print_error(r->in_file, errno); return -1; }
        dup2(fd, 0);
        close(fd);
    }
    if (r->out_file) {
        int flags = O_WRONLY | O_CREAT;
        flags |= r->append ? O_APPEND : O_TRUNC;
        int fd = open(r->out_file, flags);
        if (fd < 0) { print_error(r->out_file, errno); return -1; }
        dup2(fd, 1);
        close(fd);
    }
    return 0;
}

/* --- PATH resolution --- */

/* Search PATH for cmd; write full path to resolved. Returns 1 if found, 0 if not. */
static int resolve_command(const char *cmd, char *resolved, uint64_t buf_size) {
    /* If cmd contains '/', use as-is */
    if (strchr(cmd, '/')) {
        strncpy(resolved, cmd, buf_size - 1);
        resolved[buf_size - 1] = '\0';
        return 1;
    }

    const char *path = shell_getvar("PATH");
    if (!path) return 0;

    /* Walk PATH entries separated by ':' */
    while (*path) {
        const char *sep = strchr(path, ':');
        uint64_t dlen = sep ? (uint64_t)(sep - path) : strlen(path);
        uint64_t clen = strlen(cmd);

        if (dlen + 1 + clen < buf_size) {
            for (uint64_t i = 0; i < dlen; i++) resolved[i] = path[i];
            resolved[dlen] = '/';
            for (uint64_t i = 0; i < clen; i++) resolved[dlen + 1 + i] = cmd[i];
            resolved[dlen + 1 + clen] = '\0';

            struct stat st;
            memset(&st, 0, sizeof(st));
            if (stat(resolved, &st) == 0 && st.st_type == VFS_FILE) return 1;
        }

        if (!sep) break;
        path = sep + 1;
    }

    return 0;
}

/* Try resolve_command, then exec. Falls back to raw argv[0] if not found. */
static int exec_resolved(char *argv[]) {
    char resolved[256];
    if (resolve_command(argv[0], resolved, 256))
        return execv(resolved, argv);
    return execv(argv[0], argv);
}

/* Fork, apply redirects in child, dispatch or exec, parent waits. */
static void execute_with_redirect(int argc, char *argv[], const Redirect *r) {
    pid_t pid = fork();
    if (pid < 0) {
        print_error("fork", errno);
        return;
    }
    if (pid == 0) {
        /* Child: apply redirects */
        if (apply_redirects(r) < 0) {
            exit(1);
        }
        if (argc > 0) {
            /* Try exec first (pass argv for argument passing) */
            exec_resolved(argv);
            /* Exec failed — try as builtin */
            dispatch(argc, argv);
        }
        exit(0);
    }
    /* Parent: wait for child */
    wait(NULL);
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
static void cmd_jobs(int argc, char *argv[]);
static void cmd_fg(int argc, char *argv[]);
static void cmd_bg(int argc, char *argv[]);

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
    { "jobs",   cmd_jobs,   "List background jobs" },
    { "fg",     cmd_fg,     "Resume job in foreground" },
    { "bg",     cmd_bg,     "Resume job in background" },
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
        const char *eq = strchr(argv[0], '=');
        uint64_t nlen = (uint64_t)(eq - argv[0]);
        if (nlen >= VAR_NAME_MAX) { printf("name too long\n"); return; }
        for (uint64_t j = 0; j < nlen; j++) name[j] = argv[0][j];
        name[nlen] = '\0';
        shell_setvar(name, eq + 1);
        return;
    }

    for (int i = 0; i < (int)NUM_BUILTINS; i++) {
        if (strcmp(argv[0], builtins[i].name) == 0) {
            builtins[i].fn(argc, argv);
            return;
        }
    }
    /* Not a builtin — try fork + exec */
    pid_t pid = fork();
    if (pid < 0) { print_error("fork", errno); return; }
    if (pid == 0) {
        /* Child: create own process group */
        syscall2(SYS_SETPGID, 0, 0);
        exec_resolved(argv);
        printf("%s: command not found\n", argv[0]);
        exit(127);
    }
    /* Parent: set child pgid, give foreground, wait */
    syscall2(SYS_SETPGID, (uint64_t)pid, (uint64_t)pid);
    syscall1(SYS_TCSETPGRP, (uint64_t)pid);
    int status = 0;
    waitpid(-1, &status, 0);
    syscall1(SYS_TCSETPGRP, (uint64_t)shell_pgid);
    if (WIFSTOPPED(status)) {
        Job *j = job_alloc((int32_t)pid, (int32_t)pid, argv[0]);
        if (j) {
            j->stopped = 1;
            printf("\n[%d] Stopped    %s\n", j->job_id, argv[0]);
        }
    }
}

/* --- Command implementations --- */

static void cmd_help(int argc, char *argv[]) {
    (void)argc; (void)argv;
    puts("Available commands:");
    for (int i = 0; i < (int)NUM_BUILTINS; i++) {
        printf("  %-10s%s\n", builtins[i].name, builtins[i].help);
    }
}

static void cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) printf(" ");
        printf("%s", argv[i]);
    }
    printf("\n");
}

static void cmd_ls(int argc, char *argv[]) {
    const char *path = argc >= 2 ? argv[1] : ".";
    DirEntry entries[16];
    int64_t count = syscall3(SYS_READDIR, (uint64_t)path, (uint64_t)entries, 16);
    if (count < 0) {
        print_error(path, (int)(-count));
        return;
    }
    for (int64_t i = 0; i < count; i++) {
        /* Build full path for stat: path + "/" + name */
        char fullpath[512];
        uint64_t plen = strlen(path);
        uint64_t nlen = strlen(entries[i].name);
        if (plen + 1 + nlen >= 512) continue;

        /* Copy path */
        for (uint64_t j = 0; j < plen; j++) fullpath[j] = path[j];
        /* Add separator if needed */
        uint64_t pos = plen;
        if (plen > 0 && path[plen - 1] != '/') fullpath[pos++] = '/';
        /* Copy name */
        for (uint64_t j = 0; j < nlen; j++) fullpath[pos + j] = entries[i].name[j];
        fullpath[pos + nlen] = '\0';

        struct stat st;
        memset(&st, 0, sizeof(st));
        int sr = stat(fullpath, &st);

        /* Type + permissions */
        printf("%s", entries[i].type == VFS_DIRECTORY ? "d" : "-");
        if (sr == 0) {
            char rwx[10];
            mode_to_rwx(st.st_mode, rwx);
            printf("%s", rwx);
        } else {
            printf("---------");
        }
        printf(" ");

        if (sr == 0 && entries[i].type == VFS_FILE) {
            /* Right-align size in 8 chars */
            printf("%8ld", (long)st.st_size);
            printf(" ");
        } else {
            printf("         ");
        }

        puts(entries[i].name);
    }
}

static void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) {
        /* No file argument — read from stdin (for pipe support) */
        char buf[256];
        for (;;) {
            ssize_t n = read(0, buf, 256);
            if (n <= 0) break;
            write(1, buf, (size_t)n);
        }
        return;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { print_error(argv[1], errno); return; }

    char buf[256];
    for (;;) {
        ssize_t n = read(fd, buf, 256);
        if (n <= 0) break;
        write(1, buf, (size_t)n);
    }
    close(fd);
}

static void cmd_stat(int argc, char *argv[]) {
    if (argc < 2) { puts("usage: stat <path>"); return; }
    struct stat st;
    memset(&st, 0, sizeof(st));
    int r = stat(argv[1], &st);
    if (r < 0) { print_error(argv[1], errno); return; }

    printf("  File: %s\n", argv[1]);
    printf("  Type: %s\n", st.st_type == VFS_DIRECTORY ? "directory" : "file");
    printf(" Inode: %ld\n", (long)st.st_ino);
    printf("  Size: %ld\n", (long)st.st_size);
    printf("  Mode: 0%o\n", st.st_mode);
    char rwx[10];
    mode_to_rwx(st.st_mode, rwx);
    printf(" Perms: %s\n", rwx);
    printf(" Owner: %ld\n", (long)st.st_uid);
    printf(" Group: %ld\n", (long)st.st_gid);
}

static void cmd_mkdir(int argc, char *argv[]) {
    if (argc < 2) { puts("usage: mkdir <dir>"); return; }
    int r = mkdir(argv[1], 0755);
    if (r < 0) print_error(argv[1], errno);
}

static void cmd_rm(int argc, char *argv[]) {
    if (argc < 2) { puts("usage: rm <file>"); return; }
    int r = unlink(argv[1]);
    if (r < 0) print_error(argv[1], errno);
}

static void cmd_touch(int argc, char *argv[]) {
    if (argc < 2) { puts("usage: touch <file>"); return; }
    int fd = open(argv[1], O_CREAT | O_WRONLY);
    if (fd < 0) { print_error(argv[1], errno); return; }
    close(fd);
}

static void cmd_write(int argc, char *argv[]) {
    if (argc < 3) { puts("usage: write <file> <text...>"); return; }
    int fd = open(argv[1], O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) { print_error(argv[1], errno); return; }

    for (int i = 2; i < argc; i++) {
        if (i > 2) write(fd, " ", 1);
        write(fd, argv[i], strlen(argv[i]));
    }
    write(fd, "\n", 1);
    close(fd);
}

static void cmd_uname(int argc, char *argv[]) {
    (void)argc; (void)argv;
    puts("arc_os");
}

static void cmd_clear(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("\033[2J\033[H");
}

static void cmd_exit(int argc, char *argv[]) {
    int code = 0;
    if (argc >= 2) code = atoi(argv[1]);
    exit(code);
}

static void cmd_run(int argc, char *argv[]) {
    if (argc < 2) { puts("usage: run <path>"); return; }
    /* Resolve before fork so child doesn't need stat */
    char resolved[256];
    const char *exec_path = argv[1];
    if (resolve_command(argv[1], resolved, 256))
        exec_path = resolved;
    pid_t pid = fork();
    if (pid < 0) {
        print_error("fork", errno);
        return;
    }
    if (pid == 0) {
        /* Child: exec the binary (argv+1 so binary gets argv[0]=path) */
        execv(exec_path, argv + 1);
        /* If exec fails, exit */
        puts("exec failed");
        exit(1);
    }
    /* Parent: wait for child */
    int status = -1;
    waitpid(-1, &status, 0);
    printf("exited with status ");
    if (WIFEXITED(status))
        printf("%d", WEXITSTATUS(status));
    else
        printf("%d", status);
    printf("\n");
}

static void cmd_pid(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("PID: %d\n", (int)getpid());
}

static void cmd_cd(int argc, char *argv[]) {
    const char *path = argc >= 2 ? argv[1] : "/";
    int r = chdir(path);
    if (r < 0) print_error(path, errno);
    else refresh_cwd();
}

static void cmd_pwd(int argc, char *argv[]) {
    (void)argc; (void)argv;
    refresh_cwd();
    puts(shell_cwd);
}

static void cmd_ppid(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("PPID: %d\n", (int)getppid());
}

static void cmd_export(int argc, char *argv[]) {
    if (argc < 2) {
        /* List all variables */
        for (int i = 0; i < var_count; i++) {
            printf("%s=%s\n", var_names[i], var_values[i]);
        }
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (try_assignment(argv[i])) {
            char name[VAR_NAME_MAX];
            const char *eq = strchr(argv[i], '=');
            uint64_t nlen = (uint64_t)(eq - argv[i]);
            if (nlen >= VAR_NAME_MAX) { printf("name too long\n"); continue; }
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
        printf("%s=%s\n", var_names[i], var_values[i]);
    }
}

static void cmd_unset(int argc, char *argv[]) {
    if (argc < 2) { puts("usage: unset NAME [...]"); return; }
    for (int i = 1; i < argc; i++) {
        shell_unsetvar(argv[i]);
    }
}

static void cmd_whoami(int argc, char *argv[]) {
    (void)argc; (void)argv;
    uid_t uid = getuid();
    if (uid == 0) puts("root");
    else printf("user%d\n", (int)uid);
}

static void cmd_id(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("uid=%d gid=%d\n", (int)getuid(), (int)getgid());
}

static void cmd_chmod(int argc, char *argv[]) {
    if (argc < 3) { puts("usage: chmod <mode> <path>"); return; }
    uint32_t mode = sh_parse_octal(argv[1]);
    int r = chmod(argv[2], (mode_t)mode);
    if (r < 0) print_error(argv[2], errno);
}

static void cmd_chown(int argc, char *argv[]) {
    if (argc < 3) { puts("usage: chown <uid[:gid]> <path>"); return; }
    uint32_t uid = 0;
    gid_t gid = (gid_t)-1;
    const char *s = argv[1];
    while (*s >= '0' && *s <= '9') { uid = uid * 10 + (uint32_t)(*s - '0'); s++; }
    if (*s == ':') {
        s++; gid = 0;
        while (*s >= '0' && *s <= '9') { gid = gid * 10 + (gid_t)(*s - '0'); s++; }
    }
    int r = chown(argv[2], (uid_t)uid, gid);
    if (r < 0) print_error(argv[2], errno);
}

/* --- Job control builtins --- */

static void cmd_jobs(int argc, char *argv[]) {
    (void)argc; (void)argv;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].in_use)
            job_print(&job_table[i]);
    }
}

static void cmd_fg(int argc, char *argv[]) {
    Job *j = NULL;
    if (argc >= 2) {
        j = job_find_by_id(atoi(argv[1]));
    } else {
        /* Most recent job */
        for (int i = MAX_JOBS - 1; i >= 0; i--) {
            if (job_table[i].in_use) { j = &job_table[i]; break; }
        }
    }
    if (!j) { puts("fg: no such job"); return; }

    puts(j->cmd);

    /* Give job the foreground */
    syscall1(SYS_TCSETPGRP, (uint64_t)j->pgid);

    /* If stopped, send SIGCONT */
    if (j->stopped) {
        kill(-(pid_t)j->pgid, SIGCONT);
        j->stopped = 0;
    }

    /* Wait for it */
    int status = 0;
    waitpid(-1, &status, 0);

    /* Reclaim foreground */
    syscall1(SYS_TCSETPGRP, (uint64_t)shell_pgid);

    if (WIFSTOPPED(status)) {
        j->stopped = 1;
        printf("\n[%d] Stopped    %s\n", j->job_id, j->cmd);
    } else {
        job_free(j);
    }
}

static void cmd_bg(int argc, char *argv[]) {
    Job *j = NULL;
    if (argc >= 2) {
        j = job_find_by_id(atoi(argv[1]));
    } else {
        for (int i = MAX_JOBS - 1; i >= 0; i--) {
            if (job_table[i].in_use && job_table[i].stopped) {
                j = &job_table[i]; break;
            }
        }
    }
    if (!j) { puts("bg: no such job"); return; }
    if (!j->stopped) { puts("bg: job not stopped"); return; }

    j->stopped = 0;
    kill(-(pid_t)j->pgid, SIGCONT);
    printf("[%d] %s\n", j->job_id, j->cmd);
}

/* --- Background execution --- */

static void execute_background(char *line) {
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    if (argc == 0) return;

    Redirect redir = {0, 0, 0};
    if (parse_redirects(&argc, argv, &redir) < 0) return;

    pid_t pid = fork();
    if (pid < 0) { print_error("fork", errno); return; }
    if (pid == 0) {
        /* Child: create own process group */
        syscall2(SYS_SETPGID, 0, 0);
        if (redir.in_file || redir.out_file)
            apply_redirects(&redir);
        exec_resolved(argv);
        dispatch(argc, argv);
        exit(0);
    }
    /* Parent: set child's pgid, add to job table, do NOT wait */
    syscall2(SYS_SETPGID, (uint64_t)pid, (uint64_t)pid);
    Job *j = job_alloc((int32_t)pid, (int32_t)pid, argv[0]);
    if (j) {
        printf("[%d] %d\n", j->job_id, (int)pid);
    }
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
    return NULL;
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
    int pipefd[2];
    int err = pipe(pipefd);
    if (err < 0) {
        print_error("pipe", errno);
        return;
    }

    /* Fork child 1 (writer — runs left command, stdout -> pipe write end) */
    pid_t pid1 = fork();
    if (pid1 < 0) {
        print_error("fork", errno);
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (pid1 == 0) {
        /* Child 1: close read end, dup write end to stdout, close original */
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);

        /* Parse and run left command (with optional redirects) */
        char *argv[MAX_ARGV];
        int argc = parse_line(left, argv);
        if (argc > 0) {
            Redirect redir = {0, 0, 0};
            if (parse_redirects(&argc, argv, &redir) == 0)
                apply_redirects(&redir);
            /* Try exec first (pass argv) */
            exec_resolved(argv);
            /* Exec failed — try as builtin */
            dispatch(argc, argv);
        }
        exit(0);
    }

    /* Fork child 2 (reader — runs right command, stdin -> pipe read end) */
    pid_t pid2 = fork();
    if (pid2 < 0) {
        print_error("fork", errno);
        close(pipefd[0]);
        close(pipefd[1]);
        /* Wait for child1 */
        wait(NULL);
        return;
    }
    if (pid2 == 0) {
        /* Child 2: close write end, dup read end to stdin, close original */
        close(pipefd[1]);
        dup2(pipefd[0], 0);
        close(pipefd[0]);

        /* Parse and run right command (with optional redirects) */
        char *argv[MAX_ARGV];
        int argc = parse_line(right, argv);
        if (argc > 0) {
            Redirect redir = {0, 0, 0};
            if (parse_redirects(&argc, argv, &redir) == 0)
                apply_redirects(&redir);
            exec_resolved(argv);
            dispatch(argc, argv);
        }
        exit(0);
    }

    /* Parent: close both pipe ends, wait for both children */
    close(pipefd[0]);
    close(pipefd[1]);
    wait(NULL);
    wait(NULL);
}

/* --- Entry point --- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Initialize default variables */
    shell_setvar("PATH", "/boot");
    shell_setvar("HOME", "/");

    /* Set shell as its own process group and take foreground */
    syscall2(SYS_SETPGID, 0, 0);
    shell_pgid = (int32_t)getpid();
    syscall1(SYS_TCSETPGRP, (uint64_t)shell_pgid);
    /* Ignore SIGTSTP so shell can't be stopped */
    signal(SIGTSTP, SIG_IGN);

    puts("arc_os shell v0.2");
    puts("Type 'help' for available commands.");
    printf("\n");

    char line[LINE_MAX];
    char expanded[LINE_MAX * 2];
    char *args[MAX_ARGV];
    refresh_cwd();

    for (;;) {
        printf("[%s]$ ", shell_cwd);
        ssize_t n = read(0, line, LINE_MAX - 1);
        if (n <= 0) break;
        line[n] = '\0';

        /* Strip trailing newline */
        for (int i = 0; line[i]; i++) {
            if (line[i] == '\n') { line[i] = '\0'; break; }
        }

        /* Expand variables */
        if (expand_variables(line, expanded, sizeof(expanded)) < 0) {
            puts("expansion error: line too long");
            continue;
        }

        /* Reap completed background jobs */
        reap_background_jobs();

        /* Check for trailing & (background execution) */
        int background = 0;
        {
            int len = 0;
            while (expanded[len]) len++;
            while (len > 0 && expanded[len - 1] == ' ') len--;
            if (len > 0 && expanded[len - 1] == '&') {
                expanded[len - 1] = '\0';
                background = 1;
            }
        }

        if (background) {
            execute_background(expanded);
        } else {
            /* Check for pipe (quote-aware) */
            char *pipe_pos = find_pipe(expanded);
            if (pipe_pos != NULL) {
                *pipe_pos = '\0';
                execute_pipe(expanded, pipe_pos + 1);
            } else {
                int ac = parse_line(expanded, args);
                Redirect redir = {0, 0, 0};
                int rr = parse_redirects(&ac, args, &redir);
                if (rr < 0) continue;
                if (redir.in_file || redir.out_file) {
                    execute_with_redirect(ac, args, &redir);
                } else {
                    dispatch(ac, args);
                }
            }
        }
    }

    exit(0);
}
