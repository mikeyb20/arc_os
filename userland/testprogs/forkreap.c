/* arc_os — fork/exit/reap lifecycle test (M0-10). Test ISOs only.
 *
 * Runs in QEMU (suite "abi"). Forks and reaps 200 children, some ending with
 * exit() and some killed by SIGKILL, and checks every status word. Then it
 * makes orphans two ways and checks the kernel hands them to init:
 *   - a child exits while its own child is still running: the grandchild sees
 *     getppid() turn into init's pid;
 *   - a child exits while its own child is already a zombie: init adopts the
 *     zombie and reaps it.
 * A socket shared by fork and dup stays open until its last fd closes.
 * init reaps the orphans after this program exits; the suite's leak budgets
 * (pages_leaked, heap_leaked_bytes, both 0) show that every process, reaped
 * by its parent or by init, gave back all its memory.
 * Prints one FAIL line per failed check; exit status = number of failures. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <uapi/unistd.h>
#include <uapi/socket.h>

#define CHILDREN   200
#define SPIN_LIMIT 5000000   /* getppid() calls before giving up on adoption */

static int failures;

#define CHECK(cond)                                            \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL forkreap:%d: %s\n", __LINE__, #cond); \
            failures++;                                        \
        }                                                      \
    } while (0)

static void fork_and_reap(void) {
    for (int i = 0; i < CHILDREN; i++) {
        int killed = (i % 10 == 9);           /* every tenth child dies by signal */
        pid_t pid = fork();
        if (pid < 0) {
            printf("FAIL forkreap: fork %d failed\n", i);
            failures++;
            return;
        }
        if (pid == 0) {
            if (killed) kill(getpid(), SIGKILL);
            _exit(i & 0x7f);
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        if (killed) {
            CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        } else {
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (i & 0x7f));
        }
    }
    /* Every child is reaped: nothing is left to wait for */
    CHECK(waitpid(-1, NULL, WNOHANG) < 0);
}

/* Read one byte from fd; returns it, or -1 at EOF or error. */
static int read_byte(int fd) {
    char c;
    return read(fd, &c, 1) == 1 ? (unsigned char)c : -1;
}

/* The middle process exits first; the grandchild, still running, must be
 * adopted by init. The grandchild reports what it saw through the pipe. */
static void orphan_running(pid_t init_pid) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t mid = fork();
    if (mid == 0) {
        close(fds[0]);
        if (fork() == 0) {
            long n = 0;
            while (getppid() != init_pid && n < SPIN_LIMIT) n++;
            char c = (getppid() == init_pid) ? 'Y' : 'N';
            write(fds[1], &c, 1);
            _exit(0);
        }
        _exit(0);                                 /* orphan the grandchild */
    }
    close(fds[1]);
    int status = 0;
    CHECK(waitpid(mid, &status, 0) == mid && WIFEXITED(status));
    CHECK(read_byte(fds[0]) == 'Y');
    CHECK(read_byte(fds[0]) == -1);               /* EOF: the grandchild exited */
    close(fds[0]);
}

/* The grandchild exits first and stays a zombie (its parent never waits);
 * then the middle process exits and init must adopt and reap the zombie. */
static void orphan_zombie(void) {
    pid_t mid = fork();
    if (mid == 0) {
        int gc[2];
        if (pipe(gc) != 0) _exit(1);
        if (fork() == 0) {
            close(gc[0]);
            _exit(0);                             /* closes gc[1]: EOF below */
        }
        close(gc[1]);
        read_byte(gc[0]);                          /* returns once it is a zombie */
        _exit(0);                                  /* without reaping it */
    }
    int status = 0;
    CHECK(waitpid(mid, &status, 0) == mid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* fork and dup copy a socket fd; each copy holds it open (the kernel frees it
 * at the last close, which the leak budget checks) */
static void socket_shared(void) {
    long ret;     /* libc has no socket(): raw syscall, as in abi_check */
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"((long)__NR_socket), "D"((long)AF_INET), "S"((long)SOCK_DGRAM), "d"(0L)
                     : "rcx", "r11", "memory");
    int s = (int)ret;
    CHECK(s >= 0);
    if (s < 0) return;
    int d = dup(s);
    CHECK(d >= 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(s);
        close(d);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    struct stat st;
    CHECK(fstat(s, &st) == 0 && S_ISSOCK(st.st_mode));   /* child's close kept it */
    close(s);
    CHECK(fstat(d, &st) == 0 && S_ISSOCK(st.st_mode));   /* dup still holds it */
    close(d);
}

int main(void) {
    pid_t init_pid = getppid();                   /* init runs this program */
    fork_and_reap();
    orphan_running(init_pid);
    orphan_zombie();
    socket_shared();
    /* The orphans belong to init now, not to us */
    CHECK(waitpid(-1, NULL, WNOHANG) < 0);
    printf("forkreap: %d children reaped, %d failures\n", CHILDREN, failures);
    return failures;
}
