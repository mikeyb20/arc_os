/* arc_os — Linux syscall ABI checks (M0-05). Test ISOs only.
 *
 * Runs in QEMU (suite "abi") and checks the renumbered syscalls behave the
 * Linux way: -ENOSYS for unknown numbers, getdents64 records, struct stat
 * mode bits, wait4 pid selection and status words, rt_sigaction rules,
 * ioctl(TIOCSPGRP/TIOCGPGRP), brk, getcwd's return value, socket address checks.
 * Prints one FAIL line per failed check; exit status = number of failures.
 *
 * M0-06 adds: argument registers preserved across syscalls, arch_prctl and the
 * FS base, set_tid_address, readv/writev, termios ioctls, execve argument
 * errors, and SSE registers and FS base surviving context switches.
 *
 * Unlike normal programs it makes raw syscalls for the cases libc hides (the
 * -errno value itself, unknown numbers). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <uapi/unistd.h>
#include <uapi/socket.h>
#include <uapi/prctl.h>
#include <uapi/uio.h>
#include <uapi/termios.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL abi_check:%d: %s\n", __LINE__, #cond); failures++; } \
} while (0)

static long raw3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                      : "rcx", "r11", "memory");
    return ret;
}

static long raw4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long r10 __asm__("r10") = a3;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                      : "rcx", "r11", "memory");
    return ret;
}

static void test_std_fds(void) {
    /* init hands every process fds 0-2 on /dev/tty, so a new fd is >= 3 */
    struct stat st;
    CHECK(fstat(0, &st) == 0 && fstat(1, &st) == 0 && fstat(2, &st) == 0);
    int fd = open("/boot/shell", O_RDONLY);
    CHECK(fd >= 3);
    close(fd);
}

static void test_enosys(void) {
    CHECK(raw3(__NR_mmap, 0, 4096, 3) == -ENOSYS);      /* real number, not implemented yet */
    CHECK(raw3(511, 0, 0, 0) == -ENOSYS);               /* last table slot, empty */
    CHECK(raw3(512, 0, 0, 0) == -ENOSYS);               /* past the table */
    CHECK(raw3(100000, 0, 0, 0) == -ENOSYS);
    CHECK(raw3(-1, 0, 0, 0) == -ENOSYS);
}

/* Walk one getdents64 buffer; returns records seen, -1 on a malformed record. */
static int walk_dirents(const char *buf, long len, long *last_off, int *saw_shell, int *saw_boot) {
    int n = 0;
    long pos = 0;
    while (pos < len) {
        const struct linux_dirent64 *d = (const struct linux_dirent64 *)(buf + pos);
        if (d->d_reclen == 0 || (d->d_reclen % 8) != 0 || pos + d->d_reclen > len) return -1;
        long namelen = (long)strlen(d->d_name);
        if (19 + namelen + 1 > d->d_reclen) return -1;
        if (d->d_off <= *last_off) return -1;              /* cookies move forward */
        *last_off = d->d_off;
        if (strcmp(d->d_name, "shell") == 0 && d->d_type == DT_REG) *saw_shell = 1;
        if (strcmp(d->d_name, "boot") == 0 && d->d_type == DT_DIR) *saw_boot = 1;
        pos += d->d_reclen;
        n++;
    }
    return n;
}

static void test_getdents64(void) {
    static char big[4096] __attribute__((aligned(8)));
    static char small[64] __attribute__((aligned(8)));

    int fd = open("/boot", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    long off = 0;
    int shell = 0, boot = 0;
    long len = raw3(__NR_getdents64, fd, (long)big, sizeof(big));
    CHECK(len > 0);
    int total = walk_dirents(big, len, &off, &shell, &boot);
    CHECK(total > 5);
    CHECK(shell == 1);
    CHECK(raw3(__NR_getdents64, fd, (long)big, sizeof(big)) == 0);   /* end */
    close(fd);

    /* Same directory through a 64-byte buffer: same count, more calls */
    fd = open("/boot", O_RDONLY);
    off = 0;
    int count = 0, calls = 0;
    for (;;) {
        long n = raw3(__NR_getdents64, fd, (long)small, sizeof(small));
        if (n <= 0) { CHECK(n == 0); break; }
        int r = walk_dirents(small, n, &off, &shell, &boot);
        CHECK(r > 0);
        if (r <= 0) break;
        count += r;
        calls++;
    }
    CHECK(count == total);
    CHECK(calls > 1);
    close(fd);

    /* A buffer too small for one record */
    fd = open("/boot", O_RDONLY);
    CHECK(raw3(__NR_getdents64, fd, (long)small, 8) == -EINVAL);
    close(fd);

    /* The root lists mount points as directories */
    fd = open("/", O_RDONLY);
    off = 0;
    len = raw3(__NR_getdents64, fd, (long)big, sizeof(big));
    CHECK(walk_dirents(big, len, &off, &shell, &boot) > 0);
    CHECK(boot == 1);
    close(fd);

    /* Errors */
    fd = open("/boot/shell", O_RDONLY);
    CHECK(raw3(__NR_getdents64, fd, (long)big, sizeof(big)) == -ENOTDIR);
    close(fd);
    CHECK(raw3(__NR_getdents64, 63, (long)big, sizeof(big)) == -EBADF);
    CHECK(open("/boot/shell", O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
    CHECK(open("/boot", O_RDWR) == -1 && errno == EISDIR);

    /* libc readdir on top */
    DIR *d = opendir("/boot");
    CHECK(d != NULL);
    int via_libc = 0;
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL) via_libc++;
    if (d) closedir(d);
    CHECK(via_libc == total);
}

static void test_stat(void) {
    struct stat st;
    CHECK(sizeof(struct stat) == 144);
    CHECK(stat("/boot", &st) == 0 && S_ISDIR(st.st_mode) && st.st_nlink == 2);
    CHECK(stat("/boot/shell", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
    CHECK(st.st_blksize == 4096 && st.st_blocks == (st.st_size + 511) / 512);
    long size = st.st_size;
    int fd = open("/boot/shell", O_RDONLY);
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == size);
    close(fd);
    CHECK(stat("/no/such/file", &st) == -1 && errno == ENOENT);
    CHECK(raw3(__NR_stat, (long)"/boot", 0, 0) == -EFAULT);
    CHECK(fstat(62, &st) == -1 && errno == EBADF);
}

static void test_wait4(void) {
    int status = 0;

    /* pid selection: wait for the second child first */
    pid_t a = fork();
    if (a == 0) _exit(3);
    pid_t b = fork();
    if (b == 0) _exit(5);
    CHECK(waitpid(b, &status, 0) == b);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5 && !WIFSIGNALED(status));
    CHECK(waitpid(a, &status, 0) == a);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 3);
    CHECK(waitpid(-1, &status, 0) == -1 && errno == ECHILD);
    CHECK(waitpid(a, &status, 0) == -1 && errno == ECHILD);      /* already reaped */
    CHECK(raw4(__NR_wait4, -1, 0, 0x40000000, 0) == -EINVAL);   /* bad options */

    /* Exit status keeps only the low 8 bits */
    pid_t c = fork();
    if (c == 0) _exit(0x1234);
    CHECK(waitpid(c, &status, 0) == c && WIFEXITED(status) && WEXITSTATUS(status) == 0x34);

    /* WNOHANG with a running child returns 0 */
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    pid_t r = fork();
    if (r == 0) {
        char ch;
        close(pfd[1]);
        read(pfd[0], &ch, 1);   /* blocks until the parent closes its end */
        _exit(7);
    }
    close(pfd[0]);
    CHECK(waitpid(r, &status, WNOHANG) == 0);
    close(pfd[1]);
    CHECK(waitpid(r, &status, 0) == r && WIFEXITED(status) && WEXITSTATUS(status) == 7);

    /* Killed by a signal: WIFSIGNALED / WTERMSIG */
    pid_t k = fork();
    if (k == 0) { for (;;) getpid(); }   /* signals are delivered on syscall return */
    CHECK(kill(k, SIGTERM) == 0);
    CHECK(waitpid(k, &status, 0) == k);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM && !WIFEXITED(status));

    /* Stopped: reported only with WUNTRACED */
    pid_t s = fork();
    if (s == 0) { for (;;) getpid(); }
    CHECK(kill(s, SIGSTOP) == 0);
    CHECK(waitpid(s, &status, WUNTRACED) == s);
    CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    CHECK(waitpid(s, &status, WNOHANG) == 0);            /* no second report, no WUNTRACED */
    CHECK(kill(s, SIGCONT) == 0);
    CHECK(kill(s, SIGKILL) == 0);
    CHECK(waitpid(s, &status, 0) == s && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

static volatile int got_usr1;
static void on_usr1(int sig) { got_usr1 = sig; }

static void test_rt_sigaction(void) {
    struct kernel_sigaction act, old;
    act.sa_handler = (unsigned long)on_usr1;
    act.sa_flags = 0;
    act.sa_restorer = 0;
    act.sa_mask = 0;

    CHECK(raw4(__NR_rt_sigaction, SIGUSR1, (long)&act, 0, 4) == -EINVAL);   /* sigsetsize */
    CHECK(raw4(__NR_rt_sigaction, SIGKILL, (long)&act, 0, 8) == -EINVAL);
    CHECK(raw4(__NR_rt_sigaction, SIGSTOP, (long)&act, 0, 8) == -EINVAL);
    CHECK(raw4(__NR_rt_sigaction, 0, (long)&act, 0, 8) == -EINVAL);
    CHECK(raw4(__NR_rt_sigaction, SIGKILL, 0, (long)&old, 8) == 0);         /* query is fine */

    CHECK(raw4(__NR_rt_sigaction, SIGUSR1, (long)&act, 0, 8) == 0);
    old.sa_handler = 99;
    CHECK(raw4(__NR_rt_sigaction, SIGUSR1, 0, (long)&old, 8) == 0);
    CHECK(old.sa_handler == (unsigned long)on_usr1);

    /* Linux x86_64 needs SA_RESTORER: without one, delivery is a SIGSEGV */
    pid_t c = fork();
    if (c == 0) {
        kill(getpid(), SIGUSR1);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(c, &status, 0) == c && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    /* libc signal() sets the restorer; the handler returns through it to
     * rt_sigreturn (15) */
    CHECK(signal(SIGUSR1, on_usr1) == on_usr1);
    CHECK(kill(getpid(), SIGUSR1) == 0);
    CHECK(got_usr1 == SIGUSR1);

    /* libc signal() returns the previous handler */
    CHECK(signal(SIGUSR1, SIG_IGN) == on_usr1);
    CHECK(signal(SIGUSR1, SIG_DFL) == SIG_IGN);
    CHECK(signal(SIGKILL, SIG_IGN) == SIG_ERR && errno == EINVAL);
}

static void test_ioctl(void) {
    pid_t fg = tcgetpgrp(0);      /* fd 0 is /dev/tty */
    CHECK(fg >= 0);
    CHECK(tcsetpgrp(0, getpgid(0)) == 0);
    CHECK(tcgetpgrp(0) == getpgid(0));
    if (fg > 0) tcsetpgrp(0, fg);

    int fd = open("/boot/shell", O_RDONLY);
    CHECK(tcgetpgrp(fd) == -1 && errno == ENOTTY);
    close(fd);
    int pg = 0;
    CHECK(ioctl(0, 0x1234, &pg) == -1 && errno == ENOTTY);
    CHECK(ioctl(40, TIOCGPGRP, &pg) == -1 && errno == EBADF);

    fd = open("/dev/tty", O_RDWR);
    CHECK(fd >= 0 && tcgetpgrp(fd) >= 0);
    if (fd >= 0) close(fd);
}

static void test_brk_cwd_misc(void) {
    long cur = raw3(__NR_brk, 0, 0, 0);
    CHECK(cur > 0);
    long grown = raw3(__NR_brk, cur + 3 * 4096 + 5, 0, 0);
    CHECK(grown == cur + 3 * 4096 + 5);
    memset((void *)cur, 0xAB, 3 * 4096 + 5);                 /* new pages are mapped */
    CHECK(raw3(__NR_brk, cur, 0, 0) == cur);                  /* shrink */
    CHECK(raw3(__NR_brk, cur + 4096, 0, 0) == cur + 4096);    /* regrow: fresh zero page */
    CHECK(((volatile unsigned char *)cur)[4095] == 0 || (cur & 4095) != 0);
    CHECK(raw3(__NR_brk, cur, 0, 0) == cur);
    CHECK(raw3(__NR_brk, 0x7fffffff0000L, 0, 0) == cur);      /* into the stack: refused */
    CHECK(raw3(__NR_brk, 1, 0, 0) == cur);                    /* below start: refused */

    char buf[64];
    CHECK(chdir("/boot") == 0);
    CHECK(raw3(__NR_getcwd, (long)buf, sizeof(buf), 0) == 6 && strcmp(buf, "/boot") == 0);
    CHECK(raw3(__NR_getcwd, (long)buf, 3, 0) == -ERANGE);
    CHECK(getcwd(buf, sizeof(buf)) == buf);
    CHECK(chdir("/") == 0);

    CHECK(open("/no/such", O_RDONLY) == -1 && errno == ENOENT);
    CHECK(dup2(55, 3) == -1 && errno == EBADF);
    CHECK(raw3(__NR_write, 1, 0, 0) == 0);                    /* zero-length write */
    CHECK(raw3(__NR_read, 0, -4096, 10) == -EFAULT);          /* kernel address */

    CHECK(setpgid(0, 0) == 0 && getpgid(0) == getpid());
    CHECK(getuid() == 0 && getgid() == 0);
}

static void test_sockets(void) {
    int s = (int)raw3(__NR_socket, AF_INET, SOCK_DGRAM, 0);
    CHECK(s >= 0);
    if (s < 0) return;
    struct stat st;
    CHECK(fstat(s, &st) == 0 && S_ISSOCK(st.st_mode));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = (uint16_t)((5555 >> 8) | ((5555 & 0xFF) << 8));
    CHECK(raw3(__NR_bind, s, (long)&sa, 8) == -EINVAL);              /* short addrlen */
    sa.sin_family = AF_UNIX;
    CHECK(raw3(__NR_bind, s, (long)&sa, sizeof(sa)) == -EAFNOSUPPORT);
    sa.sin_family = AF_INET;
    CHECK(raw3(__NR_bind, s, (long)&sa, sizeof(sa)) == 0);
    int f = open("/boot/shell", O_RDONLY);
    CHECK(raw3(__NR_bind, f, (long)&sa, sizeof(sa)) == -EBADF);      /* not a socket */
    close(f);
    close(s);
}

/* --- M0-06 --- */

/* Linux preserves every register but RAX, RCX and R11 across a syscall */
static void test_regs_preserved(void) {
    unsigned long out[6];
    __asm__ volatile (
        "mov $0x1111111111111111, %%rdi\n"
        "mov $0x2222222222222222, %%rsi\n"
        "mov $0x3333333333333333, %%rdx\n"
        "mov $0x4444444444444444, %%r10\n"
        "mov $0x5555555555555555, %%r8\n"
        "mov $0x6666666666666666, %%r9\n"
        "mov $39, %%eax\n"                /* getpid */
        "syscall\n"
        "mov %%rdi, 0(%0)\n"
        "mov %%rsi, 8(%0)\n"
        "mov %%rdx, 16(%0)\n"
        "mov %%r10, 24(%0)\n"
        "mov %%r8, 32(%0)\n"
        "mov %%r9, 40(%0)\n"
        : : "r"(out)
        : "rax", "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    CHECK(out[0] == 0x1111111111111111UL);
    CHECK(out[1] == 0x2222222222222222UL);
    CHECK(out[2] == 0x3333333333333333UL);
    CHECK(out[3] == 0x4444444444444444UL);
    CHECK(out[4] == 0x5555555555555555UL);
    CHECK(out[5] == 0x6666666666666666UL);
}

static unsigned long fs_word0(void) {
    unsigned long v;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(v));
    return v;
}

static void test_arch_prctl_tid(void) {
    static unsigned long tls_block[2];
    tls_block[0] = 0xF5F5F5F5UL;
    unsigned long base = 1;
    CHECK(raw3(__NR_arch_prctl, ARCH_GET_FS, (long)&base, 0) == 0 && base == 0);
    CHECK(raw3(__NR_arch_prctl, ARCH_SET_FS, (long)tls_block, 0) == 0);
    CHECK(fs_word0() == 0xF5F5F5F5UL);
    CHECK(raw3(__NR_arch_prctl, ARCH_GET_FS, (long)&base, 0) == 0);
    CHECK(base == (unsigned long)tls_block);
    CHECK(raw3(__NR_arch_prctl, ARCH_SET_FS, (long)0xFFFF800000000000UL, 0) == -EPERM);
    CHECK(raw3(__NR_arch_prctl, ARCH_GET_FS, 0x10, 0) == -EFAULT);
    CHECK(raw3(__NR_arch_prctl, ARCH_SET_GS, 0, 0) == -EINVAL);
    CHECK(raw3(__NR_arch_prctl, ARCH_SET_FS, 0, 0) == 0);

    static int tid_word;
    CHECK(raw3(__NR_set_tid_address, (long)&tid_word, 0, 0) == getpid());
}

static void test_readv_writev(void) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    struct iovec out[3];
    out[0].iov_base = "ab";   out[0].iov_len = 2;
    out[1].iov_base = "";     out[1].iov_len = 0;
    out[2].iov_base = "cdef"; out[2].iov_len = 4;
    CHECK(raw3(__NR_writev, fds[1], (long)out, 3) == 6);
    char a[3], b[8];
    struct iovec in[2];
    in[0].iov_base = a; in[0].iov_len = 3;
    in[1].iov_base = b; in[1].iov_len = 8;
    long n = raw3(__NR_readv, fds[0], (long)in, 2);
    CHECK(n == 6);
    CHECK(memcmp(a, "abc", 3) == 0 && memcmp(b, "def", 3) == 0);
    CHECK(raw3(__NR_writev, fds[1], (long)out, -1) == -EINVAL);
    CHECK(raw3(__NR_writev, fds[1], (long)out, 1025) == -EINVAL);
    CHECK(raw3(__NR_writev, fds[1], 0x10, 1) == -EFAULT);
    CHECK(raw3(__NR_writev, fds[1], (long)out, 0) == 0);
    close(fds[0]);
    close(fds[1]);
}

static void test_termios(void) {
    struct termios t;
    CHECK(raw3(__NR_ioctl, 0, TCGETS, (long)&t) == 0);
    CHECK(t.c_lflag & 0x2);                      /* ICANON */
    CHECK(raw3(__NR_ioctl, 0, TCSETSW, (long)&t) == 0);
    struct winsize ws;
    CHECK(raw3(__NR_ioctl, 1, TIOCGWINSZ, (long)&ws) == 0);
    CHECK(ws.ws_row > 0 && ws.ws_col > 0);
    CHECK(raw3(__NR_ioctl, 0, TCGETS, 0x10) == -EFAULT);
    int fds[2];
    CHECK(pipe(fds) == 0);
    CHECK(raw3(__NR_ioctl, fds[0], TCGETS, (long)&t) == -ENOTTY);
    close(fds[0]);
    close(fds[1]);
}

/* execve fails cleanly (the process keeps running) on bad arguments */
static void test_execve_errors(void) {
    static char *argv[1200];
    CHECK(raw3(__NR_execve, (long)"/nonexistent", 0x10, 0) == -EFAULT);
    CHECK(raw3(__NR_execve, 0x10, 0, 0) == -EFAULT);
    CHECK(raw3(__NR_execve, (long)"", 0, 0) == -ENOENT);
    for (int i = 0; i < 1199; i++) argv[i] = "x";
    argv[1199] = NULL;
    CHECK(raw3(__NR_execve, (long)"/nonexistent", (long)argv, 0) == -E2BIG);
    argv[0] = "/nonexistent";
    argv[1] = NULL;
    CHECK(raw3(__NR_execve, (long)"/nonexistent", (long)argv, 0) == -ENOENT);
    CHECK(raw3(__NR_execve, (long)"/etc/passwd", (long)argv, 0) == -ENOEXEC);
}

/* Block in read() on a pipe with a pattern in XMM7 and a private FS base; the
 * other process runs in between with its own. Returns mismatches seen. */
static int ping(int rfd, int wfd, unsigned long pattern, int rounds, int first) {
    static unsigned long tls[2];
    int bad = 0;
    char c = 'x';
    CHECK(raw3(__NR_arch_prctl, ARCH_SET_FS, (long)&tls[first], 0) == 0);
    for (int i = 0; i < rounds; i++) {
        if (first || i > 0) CHECK(raw3(__NR_write, wfd, (long)&c, 1) == 1);
        unsigned long out;
        long ret;
        __asm__ volatile ("movq %[pat], %%xmm7\n"
                          "syscall\n"
                          "movq %%xmm7, %[out]\n"
                          : [out] "=r"(out), "=a"(ret)
                          : [pat] "r"(pattern), "a"(0L), "D"((long)rfd), "S"(&c), "d"(1L)
                          : "rcx", "r11", "xmm7", "memory");
        if (ret != 1 || out != pattern) bad++;
        unsigned long base = 0;
        raw3(__NR_arch_prctl, ARCH_GET_FS, (long)&base, 0);
        if (base != (unsigned long)&tls[first]) bad++;
    }
    if (!first) raw3(__NR_write, wfd, (long)&c, 1);
    raw3(__NR_arch_prctl, ARCH_SET_FS, 0, 0);
    return bad;
}

static void test_sse_and_fs_survive_switches(void) {
    int to_child[2], to_parent[2];
    CHECK(pipe(to_child) == 0 && pipe(to_parent) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        int bad = ping(to_child[0], to_parent[1], 0xC41DC41DC41DC41DUL, 20, 0);
        _exit(bad > 0 ? 1 : 0);
    }
    int bad = ping(to_parent[0], to_child[1], 0xBA5EBA5EBA5EBA5EUL, 20, 1);
    CHECK(bad == 0);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(to_child[0]); close(to_child[1]);
    close(to_parent[0]); close(to_parent[1]);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    test_std_fds();
    test_enosys();
    test_getdents64();
    test_stat();
    test_wait4();
    test_rt_sigaction();
    test_ioctl();
    test_brk_cwd_misc();
    test_sockets();
    test_regs_preserved();
    test_arch_prctl_tid();
    test_readv_writev();
    test_termios();
    test_execve_errors();
    test_sse_and_fs_survive_switches();
    printf("abi_check: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}
