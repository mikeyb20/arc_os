/* arc_os — address spaces and the mmap family under musl (M1-07). Test ISOs only.
 *
 * Built with stock musl (static), so musl's malloc, realloc (mremap) and
 * sbrk are what the kernel sees. Runs in QEMU (suite "mm"):
 *   malloc    many sizes, realloc that moves, everything checked and freed;
 *   mmap      errno for bad arguments, FIXED / FIXED_NOREPLACE, munmap that
 *             splits, mprotect, MADV_DONTNEED, mremap, MAP_32BIT, POPULATE;
 *   segv      SIGSEGV si_code SEGV_MAPERR / SEGV_ACCERR with si_addr;
 *   copy      read() into an untouched mmap buffer works, read()/write()
 *             on an unmapped address fail with EFAULT, nothing lost;
 *   brk       brk grows and shrinks a [heap] VMA;
 *   stack     the main stack grows on demand past its first 128 KiB;
 *   maps      /proc/self/maps lists the regions in Linux's format;
 *   fork      a child's pages are its own;
 *   oom       (argument "oom") a child that allocates without end is killed
 *             with SIGKILL and the system goes on.
 * Prints one FAIL line per failed check; exit status = number of failures. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int failures;

#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL mmtest:%d: %s\n", __LINE__, #cond); \
            failures++;                                      \
        }                                                    \
    } while (0)

#define PG 4096UL

static void *map(void *addr, size_t len, int prot, int flags) {
    return mmap(addr, len, prot, flags | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* --- malloc --- */

static void test_malloc(void) {
    enum { N = 512 };
    static unsigned char *p[N];
    static size_t sz[N];
    unsigned seed = 1;
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < N; i++) {
            seed = seed * 1103515245 + 12345;
            sz[i] = (seed >> 8) % ((i % 16 == 0) ? 300000 : 2000) + 1;   /* some large: mmap'd */
            p[i] = malloc(sz[i]);
            CHECK(p[i] != NULL);
            if (p[i] != NULL) memset(p[i], (int)(i + round), sz[i]);
        }
        int bad = 0;
        for (int i = 0; i < N; i++) {
            if (p[i] == NULL) continue;
            if (p[i][0] != (unsigned char)(i + round) || p[i][sz[i] - 1] != (unsigned char)(i + round)) bad++;
        }
        CHECK(bad == 0);
        for (int i = 0; i < N; i += 2) free(p[i]);
        for (int i = 1; i < N; i += 2) {
            /* grow: large blocks move with mremap */
            unsigned char *q = realloc(p[i], sz[i] * 3 + 200000);
            CHECK(q != NULL);
            if (q == NULL) continue;
            CHECK(q[0] == (unsigned char)(i + round) && q[sz[i] - 1] == (unsigned char)(i + round));
            free(q);
        }
    }
    printf("mmtest: malloc ok\n");
}

/* --- mmap family --- */

static void test_mmap_errors(void) {
    errno = 0;
    CHECK(map(NULL, 0, PROT_READ, 0) == MAP_FAILED && errno == EINVAL);
    errno = 0;
    CHECK(map((void *)0x10000001, PG, PROT_READ, MAP_FIXED) == MAP_FAILED && errno == EINVAL);
    errno = 0;
    CHECK(mmap(NULL, PG, PROT_READ, MAP_PRIVATE, -1, 0) == MAP_FAILED && errno == EBADF);
    int fd = open("/proc/self/maps", O_RDONLY);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(mmap(NULL, PG, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == ENODEV);   /* M1-12 */
    close(fd);
    errno = 0;
    CHECK(mmap(NULL, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 123) == MAP_FAILED && errno == EINVAL);
    errno = 0;
    CHECK(munmap((void *)0x10000001, PG) == -1 && errno == EINVAL);
    /* musl's mprotect() rounds the address down: ask the kernel itself */
    errno = 0;
    CHECK(syscall(SYS_mprotect, 0x10000001UL, PG, PROT_READ) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(map(NULL, 1UL << 50, PROT_READ, MAP_NORESERVE) == MAP_FAILED && errno == ENOMEM);
}

static void test_mmap_regions(void) {
    /* FIXED_NOREPLACE: once, then EEXIST */
    char *a = map((void *)0x20000000, 3 * PG, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE);
    CHECK(a == (char *)0x20000000);
    errno = 0;
    CHECK(map(a + PG, PG, PROT_READ, MAP_FIXED_NOREPLACE) == MAP_FAILED && errno == EEXIST);
    for (int i = 0; i < 3; i++) CHECK(a[i * PG] == 0);   /* zero pages */
    for (int i = 0; i < 3; i++) a[i * PG] = (char)(i + 1);
    /* FIXED replaces the middle page with a fresh one */
    CHECK(map(a + PG, PG, PROT_READ | PROT_WRITE, MAP_FIXED) == a + PG);
    CHECK(a[0] == 1 && a[PG] == 0 && a[2 * PG] == 3);
    /* munmap the middle: the ends stay */
    CHECK(munmap(a + PG, PG) == 0);
    CHECK(a[0] == 1 && a[2 * PG] == 3);
    CHECK(munmap(a, 3 * PG) == 0);   /* over the hole: fine */
    /* MADV_DONTNEED: zero again on the next touch */
    char *b = map(NULL, 4 * PG, PROT_READ | PROT_WRITE, 0);
    CHECK(b != MAP_FAILED);
    memset(b, 0x5A, 4 * PG);
    CHECK(madvise(b, 4 * PG, MADV_DONTNEED) == 0);
    CHECK(b[0] == 0 && b[4 * PG - 1] == 0);
    CHECK(madvise(b, 4 * PG, MADV_WILLNEED) == 0);
    errno = 0;
    CHECK(madvise(b, PG, 12345) == -1 && errno == EINVAL);
    /* mremap: grow in place, move with MAYMOVE, refuse without */
    b[0] = 7;
    char *c = mremap(b, 4 * PG, 8 * PG, 0);
    CHECK(c == b);
    CHECK(map(b + 8 * PG, PG, PROT_READ, MAP_FIXED) == b + 8 * PG);   /* block it */
    errno = 0;
    CHECK(mremap(b, 8 * PG, 16 * PG, 0) == MAP_FAILED && errno == ENOMEM);
    c = mremap(b, 8 * PG, 16 * PG, MREMAP_MAYMOVE);
    CHECK(c != MAP_FAILED && c != b);
    if (c != MAP_FAILED) {
        CHECK(c[0] == 7);
        c[16 * PG - 1] = 1;   /* the grown part is usable */
        CHECK(munmap(c, 16 * PG) == 0);
    }
    munmap(b + 8 * PG, PG);
    /* MAP_32BIT lands in the second GiB; POPULATE reads as zero */
    char *d = map(NULL, PG, PROT_READ | PROT_WRITE, MAP_32BIT);
    CHECK(d != MAP_FAILED && (uintptr_t)d >= 0x40000000 && (uintptr_t)d < 0x80000000);
    munmap(d, PG);
    char *e = map(NULL, 8 * PG, PROT_READ | PROT_WRITE, MAP_POPULATE);
    CHECK(e != MAP_FAILED && e[0] == 0 && e[8 * PG - 1] == 0);
    munmap(e, 8 * PG);
    /* a big NORESERVE region costs nothing until touched */
    char *f = map(NULL, 256UL << 20, PROT_READ | PROT_WRITE, MAP_NORESERVE);
    CHECK(f != MAP_FAILED);
    if (f != MAP_FAILED) {
        for (size_t off = 0; off < (256UL << 20); off += 16UL << 20) f[off] = 1;
        CHECK(f[16UL << 20] == 1);
        CHECK(munmap(f, 256UL << 20) == 0);
    }
    printf("mmtest: mmap ok\n");
}

/* --- SIGSEGV codes --- */

static sigjmp_buf segv_jmp;
static volatile int segv_code;
static volatile uintptr_t segv_addr;

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig;
    (void)uc;
    segv_code = si->si_code;
    segv_addr = (uintptr_t)si->si_addr;
    siglongjmp(segv_jmp, 1);
}

/* Touch addr (write if w); returns the si_code of the SIGSEGV, or 0. The
 * handler runs with SA_NODEFER and jumps out without restoring a mask:
 * rt_sigprocmask comes with M1-17. */
static int probe(volatile char *addr, int w) {
    segv_code = 0;
    segv_addr = 0;
    if (sigsetjmp(segv_jmp, 0) == 0) {
        if (w)
            *addr = 1;
        else
            (void)*addr;
        return 0;
    }
    return segv_code;
}

static void test_segv(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);

    char *a = map(NULL, 2 * PG, PROT_READ | PROT_WRITE, 0);
    CHECK(a != MAP_FAILED);
    CHECK(munmap(a + PG, PG) == 0);
    CHECK(probe(a + PG + 8, 0) == SEGV_MAPERR);
    CHECK(segv_addr == (uintptr_t)(a + PG + 8));
    CHECK(mprotect(a, PG, PROT_READ) == 0);
    CHECK(probe(a, 0) == 0);
    CHECK(probe(a + 16, 1) == SEGV_ACCERR);
    CHECK(segv_addr == (uintptr_t)(a + 16));
    CHECK(mprotect(a, PG, PROT_NONE) == 0);
    CHECK(probe(a, 0) == SEGV_ACCERR);
    CHECK(mprotect(a, PG, PROT_READ | PROT_WRITE) == 0);
    CHECK(probe(a, 1) == 0);
    CHECK(probe((char *)8, 0) == SEGV_MAPERR);   /* NULL page */
    munmap(a, PG);
    signal(SIGSEGV, SIG_DFL);
    printf("mmtest: segv ok\n");
}

/* --- Kernel copies to and from user memory --- */

static void test_copy(void) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    /* read() into a buffer no page backs yet: the kernel's copy faults it in */
    char *buf = map(NULL, 4 * PG, PROT_READ | PROT_WRITE, 0);
    CHECK(buf != MAP_FAILED);
    CHECK(write(fds[1], "hello", 5) == 5);
    CHECK(read(fds[0], buf + 2 * PG - 2, 5) == 5);   /* across a page boundary */
    CHECK(memcmp(buf + 2 * PG - 2, "hello", 5) == 0);
    /* an unmapped address: EFAULT, and the bytes are not lost for the file */
    char *hole = map(NULL, PG, PROT_READ | PROT_WRITE, 0);
    munmap(hole, PG);
    errno = 0;
    CHECK(write(fds[1], hole, 5) == -1 && errno == EFAULT);
    int fd = open("/proc/self/maps", O_RDONLY);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(read(fd, hole, 64) == -1 && errno == EFAULT);
    CHECK(read(fd, buf, 64) == 64);   /* the offset did not move on the failure */
    CHECK(strncmp(buf, "00", 2) == 0);
    close(fd);
    /* a read-only buffer: the kernel may not write it */
    CHECK(mprotect(buf, PG, PROT_READ) == 0);
    CHECK(write(fds[1], "x", 1) == 1);
    errno = 0;
    CHECK(read(fds[0], buf, 1) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(read(0, (void *)0xFFFF800000000000ULL, 1) == -1 && errno == EFAULT);   /* kernel half */
    close(fds[0]);
    close(fds[1]);
    munmap(buf, 4 * PG);
    printf("mmtest: copy ok\n");
}

/* --- brk ---
 * The raw syscall: musl's sbrk() refuses a nonzero increment (its malloc
 * owns the break) */

static char *brk_to(char *addr) {
    return (char *)syscall(SYS_brk, addr);
}

static void test_brk(void) {
    char *base = brk_to(0);
    CHECK(base != (char *)-1 && base != NULL);
    CHECK(brk_to(base + 5 * PG) == base + 5 * PG);
    memset(base, 0x11, 5 * PG);
    CHECK(base[5 * PG - 1] == 0x11);
    CHECK(brk_to(base + 2 * PG) == base + 2 * PG);   /* shrink */
    CHECK(base[0] == 0x11);
    CHECK(brk_to(base + 5 * PG) == base + 5 * PG);
    CHECK(base[3 * PG] == 0);                         /* given back, so zero again */
    CHECK(brk_to((char *)0x1000) == base + 5 * PG);   /* below the start: unchanged */
    CHECK(brk_to(base) == base);
    printf("mmtest: brk ok\n");
}

/* --- Stack growth --- */

static int __attribute__((noinline)) deep(int n) {
    volatile char frame[16 * 1024];
    frame[0] = (char)n;
    frame[sizeof(frame) - 1] = (char)n;
    if (n == 0) return frame[0];
    return deep(n - 1) + frame[sizeof(frame) - 1] - (char)n;
}

static void test_stack(void) {
    CHECK(deep(128) == 0);   /* about 2 MiB, past the first 128 KiB */
    printf("mmtest: stack ok\n");
}

/* --- /proc/self/maps --- */

static void test_maps(void) {
    char *m = map((void *)0x30000000, 2 * PG, PROT_READ, MAP_FIXED_NOREPLACE);
    CHECK(m == (char *)0x30000000);
    static char text[16384];
    int fd = open("/proc/self/maps", O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = 0, r;
    while (fd >= 0 && n < (ssize_t)sizeof(text) - 1 && (r = read(fd, text + n, sizeof(text) - 1 - (size_t)n)) > 0)
        n += r;
    text[n > 0 ? n : 0] = '\0';
    close(fd);
    CHECK(strstr(text, "30000000-30002000 r--p 00000000 00:00 0") != NULL);
    char *heap = strstr(text, "[heap]");
    char *stack = strstr(text, "[stack]");
    CHECK(heap != NULL && stack != NULL);
    /* the name starts at column 74: 73 characters of fields and padding */
    if (heap != NULL) {
        char *line = heap;
        while (line > text && line[-1] != '\n') line--;
        CHECK(heap - line == 73);
    }
    CHECK(strstr(text, "r-xp") != NULL);   /* program text */
    munmap(m, 2 * PG);
    printf("mmtest: maps ok\n");
}

/* --- fork --- */

static void test_fork(void) {
    char *a = map(NULL, PG, PROT_READ | PROT_WRITE, 0);
    CHECK(a != MAP_FAILED);
    a[0] = 'p';
    pid_t pid = fork();
    if (pid == 0) {
        int bad = a[0] != 'p';
        a[0] = 'c';
        _exit(bad);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(a[0] == 'p');
    munmap(a, PG);
    printf("mmtest: fork ok\n");
}

/* --- Out of memory --- */

static void test_oom(void) {
    pid_t pid = fork();
    if (pid == 0) {
        for (;;) {
            char *p = map(NULL, 4UL << 20, PROT_READ | PROT_WRITE, MAP_NORESERVE);
            if (p == MAP_FAILED) _exit(2);
            for (size_t off = 0; off < (4UL << 20); off += PG) p[off] = 1;
        }
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    /* the memory came back: a fresh allocation works */
    char *p = map(NULL, 16UL << 20, PROT_READ | PROT_WRITE, 0);
    CHECK(p != MAP_FAILED);
    if (p != MAP_FAILED) {
        memset(p, 1, 16UL << 20);
        munmap(p, 16UL << 20);
    }
    printf("mmtest: oom ok (child killed by signal %d)\n", WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "oom") == 0) {
        test_oom();
    } else {
        test_malloc();
        test_mmap_errors();
        test_mmap_regions();
        test_segv();
        test_copy();
        test_brk();
        test_stack();
        test_maps();
        test_fork();
    }
    printf("mmtest: %d failures\n", failures);
    return failures;
}
