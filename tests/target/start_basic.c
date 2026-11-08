/* arc_os M0-06 test program: Linux process start (ADR-0002).
 *
 * Built three times: with stock `musl-gcc -static` (start_musl), as musl
 * static-PIE (start_musl_pie) and with libarc (start_arc). Checks argc/argv,
 * envp, the auxiliary vector, initial stack alignment, printf, file I/O and
 * getenv; the musl builds also check TLS (arch_prctl), isatty (TIOCGWINSZ),
 * termios (TCGETS) and writev. No malloc and no signal handlers: those need
 * mmap (M1-07) and the Linux signal frame (M1-01).
 *
 * Run as: <prog> alpha beta   (from qemu suite `musl`; init passes HOME=/ and
 * TERM=linux). Exit status = number of failed checks. */

#define _GNU_SOURCE   /* environ under -std=c11 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/stat.h>

#ifdef ARCHOS_LIBC_STDLIB_H
#define LIBARC 1        /* libarc: no TLS, isatty, termios or writev yet */
#else
#define LIBARC 0
#include <errno.h>
#include <termios.h>
#include <sys/uio.h>
#endif

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
} while (0)

#if !LIBARC
static __thread int tls_value = 42;
static __thread int tls_zero;
#endif

/* ELF64 program header, enough to check AT_PHDR */
typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

static void check_args(int argc, char **argv, char **envp) {
    CHECK(argc == 3);
    if (argc == 3) {
        CHECK(strcmp(argv[1], "alpha") == 0);
        CHECK(strcmp(argv[2], "beta") == 0);
    }
    CHECK(argv[argc] == NULL);
    /* argc sits just below argv[0] at the 16-byte aligned entry RSP */
    CHECK(((uintptr_t)argv - 8) % 16 == 0);
    CHECK(envp == argv + argc + 1);
    CHECK(envp == environ);

    int envc = 0;
    while (envp[envc] != NULL) envc++;
    CHECK(envc >= 2);
    const char *home = getenv("HOME");
    const char *term = getenv("TERM");
    CHECK(home != NULL && strcmp(home, "/") == 0);
    CHECK(term != NULL && strcmp(term, "linux") == 0);
    CHECK(getenv("ARC_NO_SUCH_VARIABLE") == NULL);
}

static void check_auxv(char **argv) {
    CHECK(getauxval(AT_PAGESZ) == 4096);
    CHECK(getauxval(AT_PHENT) == sizeof(Phdr));
    unsigned long phnum = getauxval(AT_PHNUM);
    const Phdr *ph = (const Phdr *)getauxval(AT_PHDR);
    CHECK(phnum > 0 && ph != NULL);
    int loads = 0;
    for (unsigned long i = 0; ph != NULL && i < phnum; i++) {
        if (ph[i].p_type == 1) loads++;           /* PT_LOAD */
        CHECK(ph[i].p_type != 3);                 /* no PT_INTERP: static */
    }
    CHECK(loads > 0);
    CHECK(getauxval(AT_ENTRY) != 0);
    CHECK(getauxval(AT_BASE) == 0);
    CHECK(getauxval(AT_UID) == (unsigned long)getuid());
    CHECK(getauxval(AT_GID) == (unsigned long)getgid());
    CHECK(getauxval(AT_SECURE) == 0);
    unsigned long hwcap = getauxval(AT_HWCAP);
    CHECK((hwcap >> 25) & 1);                     /* SSE */
    CHECK((hwcap >> 26) & 1);                     /* SSE2 */

    const unsigned char *rnd = (const unsigned char *)getauxval(AT_RANDOM);
    CHECK(rnd != NULL);
    int nonzero = 0;
    for (int i = 0; rnd != NULL && i < 16; i++) nonzero |= rnd[i];
    CHECK(nonzero);

    const char *execfn = (const char *)getauxval(AT_EXECFN);
    CHECK(execfn != NULL && strcmp(execfn, argv[0]) == 0);
    const char *platform = (const char *)getauxval(AT_PLATFORM);
    CHECK(platform != NULL && strcmp(platform, "x86_64") == 0);
}

static void check_files(void) {
    static const char text[] = "arc_os process start\n";
    static char buf[64];
    mkdir("/tmp", 0755);   /* may exist already */
    int fd = open("/tmp/start_basic.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 3);        /* 0-2 are inherited */
    if (fd < 0) return;
    CHECK(write(fd, text, sizeof(text) - 1) == (ssize_t)(sizeof(text) - 1));
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == (ssize_t)(sizeof(text) - 1));
    CHECK(memcmp(buf, text, sizeof(text) - 1) == 0);
    CHECK(close(fd) == 0);
    CHECK(unlink("/tmp/start_basic.txt") == 0);
    CHECK(open("/tmp/start_basic.txt", O_RDONLY) < 0);
}

static void check_printf(void) {
    static char out[64];
    int n = snprintf(out, sizeof(out), "%d %s %x %lu", -7, "str", 0xbeef, 123456789UL);
    CHECK(n == 21);
    CHECK(strcmp(out, "-7 str beef 123456789") == 0);
}

#if !LIBARC
static void check_musl_only(void) {
    /* TLS through the FS base set by arch_prctl(ARCH_SET_FS) */
    CHECK(tls_value == 42);
    CHECK(tls_zero == 0);
    tls_value++;
    CHECK(tls_value == 43);

    /* Terminal: isatty() uses TIOCGWINSZ, tcgetattr() TCGETS */
    CHECK(isatty(1) == 1);
    struct termios t;
    CHECK(tcgetattr(1, &t) == 0);
    CHECK((t.c_lflag & ICANON) != 0);
    CHECK(tcsetattr(1, TCSANOW, &t) == 0);
    int fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0 && isatty(fd) == 0 && errno == ENOTTY);
    if (fd >= 0) close(fd);

    /* writev straight to stdout */
    struct iovec iov[2] = {
        { .iov_base = "writev ", .iov_len = 7 },
        { .iov_base = "ok\n",    .iov_len = 3 },
    };
    CHECK(writev(1, iov, 2) == 10);
}
#endif

int main(int argc, char **argv, char **envp) {
    check_args(argc, argv, envp);
    check_auxv(argv);
    check_printf();
    check_files();
#if !LIBARC
    check_musl_only();
#endif
    printf("start_basic(%s): %s (%d failures)\n", LIBARC ? "libarc" : "musl",
           failures ? "FAIL" : "PASS", failures);
    return failures;
}
