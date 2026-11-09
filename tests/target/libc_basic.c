/* arc_os target program (M0-04): everyday libc use under musl.
 *
 * Exercises stdio, files, directories, environment, signals and time, the way a
 * ported command-line program does. Built statically with musl; run under strace
 * for the syscall inventory now, and as a QEMU test once arc_os runs musl
 * programs (M0-06). Exit status 0 = every check passed. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static volatile sig_atomic_t got_usr1;
static void on_usr1(int sig) { (void)sig; got_usr1 = 1; }

static void test_stdio_and_files(const char *dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/file.txt", dir);

    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    for (int i = 0; i < 1000; i++) fprintf(f, "line %d\n", i);
    CHECK(fclose(f) == 0);

    f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f) return;
    char line[64];
    int n = 0;
    while (fgets(line, sizeof(line), f)) n++;
    CHECK(n == 1000);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    CHECK(ftell(f) > 0);
    fclose(f);

    int fd = open(path, O_RDWR | O_APPEND | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(write(fd, "tail\n", 5) == 5);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    char buf[16];
    CHECK(pread(fd, buf, 6, 0) == 6 && memcmp(buf, "line 0", 6) == 0);
    CHECK(fsync(fd) == 0);
    CHECK(ftruncate(fd, 10) == 0);
    int fd2 = dup(fd);
    CHECK(fd2 > fd);
    CHECK(fcntl(fd2, F_GETFD) >= 0);
    close(fd2);
    close(fd);

    char renamed[256];
    snprintf(renamed, sizeof(renamed), "%s/renamed.txt", dir);
    CHECK(rename(path, renamed) == 0);
    CHECK(stat(renamed, &st) == 0 && st.st_size == 10);
    CHECK(access(renamed, R_OK) == 0);
    CHECK(chmod(renamed, 0600) == 0);
    CHECK(unlink(renamed) == 0);
    CHECK(stat(renamed, &st) == -1 && errno == ENOENT);
}

static void test_directories(const char *dir) {
    char sub[256];
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    CHECK(mkdir(sub, 0755) == 0);
    for (int i = 0; i < 5; i++) {
        char p[300];
        snprintf(p, sizeof(p), "%s/f%d", sub, i);
        int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(fd >= 0);
        close(fd);
    }
    DIR *d = opendir(sub);
    CHECK(d != NULL);
    int count = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') count++;
    }
    if (d) closedir(d);
    CHECK(count == 5);

    char cwd[256];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    CHECK(chdir(sub) == 0);
    CHECK(unlink("f0") == 0);
    CHECK(chdir(cwd) == 0);
    for (int i = 1; i < 5; i++) {
        char p[300];
        snprintf(p, sizeof(p), "%s/f%d", sub, i);
        unlink(p);
    }
    CHECK(rmdir(sub) == 0);
}

static void test_environment(void) {
    CHECK(setenv("ARC_TEST_VAR", "42", 1) == 0);
    const char *v = getenv("ARC_TEST_VAR");
    CHECK(v && strcmp(v, "42") == 0);
    CHECK(unsetenv("ARC_TEST_VAR") == 0);
    CHECK(getenv("ARC_TEST_VAR") == NULL);
    CHECK(getpid() > 0 && getppid() >= 0);
    CHECK(getuid() == geteuid());
}

static void test_signals_and_processes(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    CHECK(raise(SIGUSR1) == 0);
    CHECK(got_usr1 == 1);

    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0);
    CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);

    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        _exit(write(fds[1], "child", 5) == 5 ? 7 : 1);
    }
    close(fds[1]);
    char buf[8] = {0};
    CHECK(read(fds[0], buf, sizeof(buf)) == 5 && strcmp(buf, "child") == 0);
    close(fds[0]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);

    pid = fork();
    if (pid == 0) {
        /* ARC_TRUE names a static musl `true` for lane tests (M2-01), so the traced
         * child is an arc_os-bound program and not the host's dynamic /bin/true. */
        char *path = getenv("ARC_TRUE");
        char *argv[] = {path ? path : "/bin/true", NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
}

static void test_time(void) {
    struct timespec a, b, req = { 0, 10 * 1000 * 1000 };
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    CHECK(nanosleep(&req, NULL) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    CHECK(b.tv_sec > a.tv_sec || b.tv_nsec > a.tv_nsec);
    CHECK(clock_gettime(CLOCK_REALTIME, &a) == 0);
    struct timeval tv;
    CHECK(gettimeofday(&tv, NULL) == 0);
    time_t now = time(NULL);
    struct tm tm;
    CHECK(gmtime_r(&now, &tm) != NULL);
    char text[64];
    CHECK(strftime(text, sizeof(text), "%Y-%m-%d", &tm) == 10);
}

int main(void) {
    char dir[] = "/tmp/arc_libc_XXXXXX";
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    test_stdio_and_files(dir);
    test_directories(dir);
    test_environment();
    test_signals_and_processes();
    test_time();
    CHECK(rmdir(dir) == 0);

    printf("libc_basic: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
