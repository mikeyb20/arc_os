/* arc_os — Init process (PID 1)
 *
 * Normal boot: exec the login program (falls back to the shell).
 *
 * Test mode (kernel command line has arc.test=<suite>, see M0-02): run every
 * program listed in /boot/<suite>.suite, print one ARC_TEST line per program,
 * then exit 0 only if all passed. The kernel turns init's exit status into
 * QEMU's exit code.
 *
 * Suite file format, one test per line ('#' starts a comment line):
 *     <name> <path> [args...] [< <stdin-file>]
 * A test passes when the program exits with status 0.
 *
 * Leak check (M0-08, ADR-0019): /proc/meminfo is read before the first test and
 * after the last. Every child has been reaped by then, so free memory should be
 * back where it started; init prints the difference as ARC_TEST budget lines
 * that tools/budget-check.py compares with tests/budgets.txt. */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>

#define SUITE_FILE_MAX 4096
#define SUITE_NAME_MAX 64
#define TEST_ARGS_MAX  16

/* Read a whole small file into buf (NUL-terminated). Returns length or -1. */
static int read_file(const char *path, char *buf, int size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int total = 0;
    while (total < size - 1) {
        int n = (int)read(fd, buf + total, (size_t)(size - 1 - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

/* Find arc.test=<suite> in /proc/cmdline. Returns 1 and fills suite if found. */
static int find_test_suite(char *suite, int size) {
    char line[256];
    if (read_file("/proc/cmdline", line, sizeof(line)) <= 0) return 0;

    const char *key = "arc.test=";
    int klen = (int)strlen(key);
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, (size_t)klen) == 0 && (p == line || p[-1] == ' ' || p[-1] == '\t')) {
            p += klen;
            int n = 0;
            while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' && n < size - 1) {
                suite[n] = p[n];
                n++;
            }
            suite[n] = '\0';
            return n > 0;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return 0;
}

/* Split a line into whitespace-separated words, in place. Returns word count. */
static int split_words(char *line, char **words, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        words[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* Run one test line. Returns 1 if it passed. */
static int run_test(char *line) {
    char *words[TEST_ARGS_MAX + 4];
    int n = split_words(line, words, TEST_ARGS_MAX + 3);
    if (n < 2) {
        printf("ARC_TEST FAIL %s bad-suite-line\n", n > 0 ? words[0] : "?");
        return 0;
    }

    const char *name = words[0];
    const char *stdin_file = NULL;
    /* Trailing "< file" redirects the program's stdin */
    if (n >= 4 && strcmp(words[n - 2], "<") == 0) {
        stdin_file = words[n - 1];
        n -= 2;
    }
    char *argv[TEST_ARGS_MAX + 1];
    int argc = 0;
    for (int i = 1; i < n && argc < TEST_ARGS_MAX; i++) argv[argc++] = words[i];
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        printf("ARC_TEST FAIL %s fork-failed\n", name);
        return 0;
    }
    if (pid == 0) {
        if (stdin_file != NULL) {
            int fd = open(stdin_file, O_RDONLY);
            if (fd < 0) exit(126);
            if (fd != 0) {   /* init has no stdin fd, so open may return 0 */
                dup2(fd, 0);
                close(fd);
            }
        }
        execv(argv[0], argv);
        exit(127);   /* exec failed */
    }

    int status = 0;
    pid_t got = waitpid(pid, &status, 0);
    /* Reap orphans the test left behind: the kernel hands them to init (us),
     * and an unreaped zombie would show up as leaked memory */
    int orphan_status;
    while (waitpid(-1, &orphan_status, WNOHANG) > 0) {
    }
    if (got < 0) {
        printf("ARC_TEST FAIL %s wait-failed\n", name);
        return 0;
    }
    /* A child killed by a signal reports 128 + signal, like a shell does */
    int code = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
    if (code == 0) {
        printf("ARC_TEST PASS %s\n", name);
        return 1;
    }
    if (code == 127) {
        printf("ARC_TEST FAIL %s exec-failed:%s\n", name, argv[0]);
    } else if (code == 126) {
        printf("ARC_TEST FAIL %s stdin-open-failed:%s\n", name, stdin_file);
    } else {
        printf("ARC_TEST FAIL %s exit=%d\n", name, code);
    }
    return 0;
}

typedef struct {
    long free_kb;       /* MemFree */
    long heap_used;     /* HeapUsed + HeapLarge (bytes of live kmalloc blocks) */
    long heap_mapped;   /* HeapMapped + HeapLarge (bytes of pages the kernel heap holds) */
} MemSnap;

/* Value after "<key>:" in a /proc/meminfo text, or -1 */
static long meminfo_field(const char *text, const char *key) {
    const char *p = strstr(text, key);
    if (p == NULL) return -1;
    p += strlen(key);
    while (*p == ':' || *p == ' ') p++;
    long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

static int mem_snap(MemSnap *m) {
    /* Empty the dentry cache first (M1-11): the entries and the disk inodes
     * they hold are a cache, not a leak */
    int dc = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (dc >= 0) {
        write(dc, "2\n", 2);
        close(dc);
    }
    char text[512];
    if (read_file("/proc/meminfo", text, sizeof(text)) <= 0) return -1;
    m->free_kb = meminfo_field(text, "MemFree");
    m->heap_used = meminfo_field(text, "HeapUsed");
    m->heap_mapped = meminfo_field(text, "HeapMapped");
    /* Large kmalloc blocks (8 KiB-4 MiB, M1-02) come straight from the page
     * allocator: they are live heap bytes and pages the heap holds */
    long large = meminfo_field(text, "HeapLarge");
    if (m->free_kb < 0 || m->heap_used < 0 || m->heap_mapped < 0 || large < 0) return -1;
    m->heap_used += large;
    m->heap_mapped += large;
    return 0;
}

/* Pages that left the free pool for good (pages the kernel heap grew by are
 * excluded: the heap keeps them) and kmalloc bytes still held */
static void report_leaks(const MemSnap *before) {
    MemSnap after;
    if (mem_snap(&after) != 0) {
        printf("ARC_TEST FAIL leak-check cannot-read-meminfo\n");
        return;
    }
    long lost_bytes = (before->free_kb - after.free_kb) * 1024
                      - (after.heap_mapped - before->heap_mapped);
    printf("ARC_TEST budget pages_leaked %ld\n", lost_bytes / 4096);
    printf("ARC_TEST budget heap_leaked_bytes %ld\n", after.heap_used - before->heap_used);
}

static int run_suite(const char *suite) {
    static char buf[SUITE_FILE_MAX];
    char path[SUITE_NAME_MAX + 16];
    snprintf(path, sizeof(path), "/boot/%s.suite", suite);

    printf("ARC_TEST START suite=%s\n", suite);
    if (read_file(path, buf, sizeof(buf)) < 0) {
        printf("ARC_TEST FAIL suite-file cannot-open:%s\n", path);
        printf("ARC_TEST DONE pass=0 fail=1\n");
        return 1;
    }

    MemSnap before;
    int have_snap = (mem_snap(&before) == 0);

    int pass = 0, fail = 0;
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0' || *line == '#') continue;
        if (run_test(line)) pass++; else fail++;
    }
    if (have_snap) report_leaks(&before);

    printf("ARC_TEST DONE pass=%d fail=%d\n", pass, fail);
    return (fail == 0 && pass > 0) ? 0 : 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    char suite[SUITE_NAME_MAX];
    if (find_test_suite(suite, sizeof(suite))) {
        exit(run_suite(suite));
    }

    /* Try login first */
    char *login_argv[] = { "/boot/login", NULL };
    execv("/boot/login", login_argv);
    /* Fall back to shell if login not found */
    char *sh_argv[] = { "/boot/shell", NULL };
    execv("/boot/shell", sh_argv);
    exit(1);
    return 1;
}
