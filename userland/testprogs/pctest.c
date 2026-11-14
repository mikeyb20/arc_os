/* arc_os — the page cache and file mmap (M1-12), under musl. Test ISOs only.
 *
 * Built with stock musl (static). Runs in QEMU (suite "pagecache"):
 *   coherent  write() then read through a MAP_SHARED mapping and the
 *             reverse; two mappings at different offsets share pages; the
 *             open-mode checks (EACCES) of mmap;
 *   private   MAP_PRIVATE writes never reach the file or a shared mapping;
 *   fork      a child's shared mapping writes reach the parent; a private
 *             copy made before fork stays the child's own;
 *   sync      nr_dirty moves with write() and fsync; msync and fdatasync
 *             arguments as Linux;
 *   sigbus    a mapped page past the end (after mmap or after truncate)
 *             raises SIGBUS with BUS_ADRERR; the rest of a truncated page
 *             reads as zero;
 *   fat       (argument "fat") the FAT32 test disk read through the cache:
 *             pgpgin and readahead counters move, WILLNEED reads ahead,
 *             private mappings copy, shared writable ones are refused;
 *   big       a 1 GiB ramfs file mapped and summed by 4 processes at once:
 *             the pages are shared (Mapped about 1 GiB, MemFree drops by
 *             page tables only), and the file goes away when unlinked.
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL pctest:%d: %s (errno %d)\n", __LINE__, #cond, errno); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define PG 4096L

/* --- helpers --- */

/* The value after "key" (a whole word) in a /proc text file, or -1 */
static long proc_field(const char *path, const char *key) {
    static char text[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) return -1;
    text[n] = '\0';
    size_t klen = strlen(key);
    for (char *p = text; (p = strstr(p, key)) != NULL; p += klen) {
        if (p != text && p[-1] != '\n') continue;
        char *v = p + klen;
        if (*v != ':' && *v != ' ') continue;
        while (*v == ':' || *v == ' ') v++;
        return strtol(v, NULL, 10);
    }
    return -1;
}

static long vmstat(const char *key) {
    return proc_field("/proc/vmstat", key);
}

static long meminfo(const char *key) {
    return proc_field("/proc/meminfo", key);
}

static void drop_caches(void) {
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(write(fd, "3\n", 2) == 2);
    close(fd);
}

/* A new file of `pages` pages, byte i = (i * 7 + seed) & 0xFF */
static int make_file(const char *path, long pages, int seed) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return -1;
    static unsigned char buf[PG];
    for (long p = 0; p < pages; p++) {
        for (long i = 0; i < PG; i++) buf[i] = (unsigned char)(((p * PG + i) * 7 + seed) & 0xFF);
        CHECK(write(fd, buf, PG) == PG);
    }
    return fd;
}

static unsigned char expect_byte(long off, int seed) {
    return (unsigned char)((off * 7 + seed) & 0xFF);
}

static int range_matches(const unsigned char *p, long off, long len, int seed) {
    for (long i = 0; i < len; i++) {
        if (p[i] != expect_byte(off + i, seed)) return 0;
    }
    return 1;
}

/* --- coherent --- */

static void pc_coherent(void) {
    const char *path = "/tmp/pc_coh";
    int fd = make_file(path, 3, 1);
    if (fd < 0) return;
    unsigned char *m = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED) return;
    CHECK(range_matches(m, 0, 3 * PG, 1));   /* write() then read through the mapping */

    /* write() into a page that is mapped: the mapping sees it at once */
    CHECK(pwrite(fd, "written", 7, PG + 10) == 7);
    CHECK(memcmp(m + PG + 10, "written", 7) == 0);

    /* a store through the mapping: read() sees it */
    memcpy(m + 2 * PG + 100, "stored", 6);
    char buf[16] = {0};
    CHECK(pread(fd, buf, 6, 2 * PG + 100) == 6 && memcmp(buf, "stored", 6) == 0);

    /* a second mapping one page in is the same pages */
    unsigned char *m2 = mmap(NULL, 2 * PG, PROT_READ, MAP_SHARED, fd, PG);
    CHECK(m2 != MAP_FAILED);
    if (m2 != MAP_FAILED) {
        CHECK(memcmp(m2 + 10, "written", 7) == 0);
        m[PG + 2000] = 0x5A;
        CHECK(m2[2000] == 0x5A);
        CHECK(munmap(m2, 2 * PG) == 0);
    }

    /* growing the file with write() shows up in a mapping that already covers it */
    CHECK(munmap(m, 3 * PG) == 0);
    m = mmap(NULL, 4 * PG, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    if (m != MAP_FAILED) {
        CHECK(pwrite(fd, "tail", 4, 3 * PG + 5) == 4);
        CHECK(memcmp(m + 3 * PG + 5, "tail", 4) == 0 && m[3 * PG] == 0);
        CHECK(munmap(m, 4 * PG) == 0);
    }
    close(fd);

    /* open modes: shared + PROT_WRITE needs O_RDWR; nothing maps O_WRONLY */
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EACCES);
    void *p = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);   /* a private copy may */
    CHECK(p != MAP_FAILED);
    if (p != MAP_FAILED) munmap(p, PG);
    p = mmap(NULL, PG, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    if (p != MAP_FAILED) {
        /* mprotect can't add write to a shared mapping of an O_RDONLY file */
        errno = 0;
        CHECK(mprotect(p, PG, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
        munmap(p, PG);
    }
    close(fd);
    fd = open(path, O_WRONLY);
    errno = 0;
    CHECK(mmap(NULL, PG, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EACCES);
    close(fd);
    errno = 0;
    CHECK(mmap(NULL, PG, PROT_READ, MAP_SHARED, 99, 0) == MAP_FAILED && errno == EBADF);
    CHECK(unlink(path) == 0);
    if (failures == 0) printf("pctest: coherent ok\n");
}

/* --- private --- */

static void pc_private(void) {
    int before = failures;
    const char *path = "/tmp/pc_priv";
    int fd = make_file(path, 2, 2);
    if (fd < 0) return;
    unsigned char *pm = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    unsigned char *sm = mmap(NULL, 2 * PG, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(pm != MAP_FAILED && sm != MAP_FAILED);
    if (pm == MAP_FAILED || sm == MAP_FAILED) return;
    CHECK(range_matches(pm, 0, 2 * PG, 2));
    memset(pm + 100, 0xEE, 200);   /* copy on write: page 0 is now the process's own */
    CHECK(pm[100] == 0xEE && pm[299] == 0xEE);
    CHECK(range_matches(sm, 0, 2 * PG, 2));   /* the file and a shared mapping never see it */
    unsigned char buf[PG];
    CHECK(pread(fd, buf, PG, 0) == PG && range_matches(buf, 0, PG, 2));
    CHECK(msync(pm, 2 * PG, MS_SYNC) == 0);   /* msync of a private mapping writes nothing */
    CHECK(fsync(fd) == 0);
    CHECK(pread(fd, buf, PG, 0) == PG && range_matches(buf, 0, PG, 2));
    /* the copied page no longer follows the file */
    CHECK(pwrite(fd, "new", 3, 50) == 3);
    CHECK(pm[50] == expect_byte(50, 2) && memcmp(sm + 50, "new", 3) == 0);
    CHECK(munmap(pm, 2 * PG) == 0);
    /* a new private mapping sees the file as it is now, not the old copy */
    pm = mmap(NULL, PG, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(pm != MAP_FAILED);
    if (pm != MAP_FAILED) {
        CHECK(memcmp(pm + 50, "new", 3) == 0 && pm[100] == expect_byte(100, 2));
        munmap(pm, PG);
    }
    munmap(sm, 2 * PG);
    close(fd);
    CHECK(unlink(path) == 0);
    if (failures == before) printf("pctest: private ok\n");
}

/* --- fork --- */

static void pc_fork(void) {
    int before = failures;
    const char *path = "/tmp/pc_fork";
    int fd = make_file(path, 2, 3);
    if (fd < 0) return;
    unsigned char *sm = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char *pm = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(sm != MAP_FAILED && pm != MAP_FAILED);
    if (sm == MAP_FAILED || pm == MAP_FAILED) return;
    (void)*(volatile unsigned char *)sm;         /* page 0 mapped before fork */
    pm[0] = 0x11;                                 /* private page 0 copied before fork */
    (void)*(volatile unsigned char *)(pm + PG);   /* private page 1 still the file's */
    pid_t pid = fork();
    if (pid == 0) {
        int bad = 0;
        if (pm[0] != 0x11) bad |= 1;                      /* the copy came along */
        if (pm[PG + 1] != expect_byte(PG + 1, 3)) bad |= 2;   /* the file page too */
        if (sm[1] != expect_byte(1, 3)) bad |= 4;
        sm[PG + 7] = 0x77;   /* reaches the parent */
        pm[1] = 0x22;        /* stays here */
        pm[PG + 2] = 0x33;   /* copied here */
        _exit(bad);
    }
    int st = 0;
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(sm[PG + 7] == 0x77);
    CHECK(pm[1] == expect_byte(1, 3) && pm[0] == 0x11);
    CHECK(pm[PG + 2] == expect_byte(PG + 2, 3));
    unsigned char b;
    CHECK(pread(fd, &b, 1, PG + 7) == 1 && b == 0x77);
    munmap(sm, 2 * PG);
    munmap(pm, 2 * PG);
    close(fd);
    CHECK(unlink(path) == 0);
    if (failures == before) printf("pctest: fork ok\n");
}

/* --- sync --- */

static void pc_sync(void) {
    int before = failures;
    const char *path = "/tmp/pc_sync";
    long d0 = vmstat("nr_dirty");
    int fd = make_file(path, 4, 4);
    if (fd < 0) return;
    long d1 = vmstat("nr_dirty");
    CHECK(d0 >= 0 && d1 >= d0 + 4);   /* write() dirties the pages */
    CHECK(fsync(fd) == 0);
    long d2 = vmstat("nr_dirty");
    CHECK(d2 <= d1 - 4);
    unsigned char *m = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED) return;
    CHECK(vmstat("nr_dirty") == d2);   /* reading through the mapping dirties nothing */
    CHECK(m[3] == expect_byte(3, 4));
    CHECK(vmstat("nr_dirty") == d2);
    m[PG] = 1;
    m[3 * PG] = 1;
    CHECK(vmstat("nr_dirty") == d2 + 2);   /* a store marks its page dirty */
    CHECK(msync(m, 4 * PG, MS_SYNC) == 0);
    CHECK(vmstat("nr_dirty") == d2);
    m[3 * PG + 1] = 2;   /* writable again after the write-back */
    CHECK(vmstat("nr_dirty") == d2 + 1);
    CHECK(msync(m, 4 * PG, MS_ASYNC) == 0);
    CHECK(fdatasync(fd) == 0);
    CHECK(vmstat("nr_dirty") == d2);
    CHECK(vmstat("nr_mapped") >= 2);

    errno = 0;
    CHECK(msync(m, PG, MS_SYNC | MS_ASYNC) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(msync(m + 1, PG, MS_SYNC) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(msync(m, PG, 8) == -1 && errno == EINVAL);
    CHECK(msync(m, PG, MS_INVALIDATE) == 0);
    CHECK(msync(m, 0, MS_SYNC) == 0);
    CHECK(munmap(m + 2 * PG, PG) == 0);
    errno = 0;
    CHECK(msync(m, 4 * PG, MS_SYNC) == -1 && errno == ENOMEM);   /* a hole */
    munmap(m, 4 * PG);
    errno = 0;
    CHECK(fdatasync(99) == -1 && errno == EBADF);
    close(fd);
    CHECK(unlink(path) == 0);
    if (failures == before) printf("pctest: sync ok\n");
}

/* --- sigbus --- */

static sigjmp_buf bus_jmp;
static volatile int bus_code;
static volatile uintptr_t bus_addr;

static void on_bus(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)uc;
    bus_code = si->si_code;
    bus_addr = (uintptr_t)si->si_addr;
    siglongjmp(bus_jmp, 1);
}

/* Read addr; returns the si_code of the SIGBUS, or 0 */
static int probe_bus(volatile unsigned char *addr) {
    bus_code = 0;
    bus_addr = 0;
    if (sigsetjmp(bus_jmp, 0) == 0) {
        (void)*addr;
        return 0;
    }
    return bus_code;
}

static void pc_sigbus(void) {
    int before = failures;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_bus;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    CHECK(sigaction(SIGBUS, &sa, NULL) == 0);

    const char *path = "/tmp/pc_bus";
    int fd = make_file(path, 2, 5);
    if (fd < 0) return;
    /* a mapping longer than the file: the page past the end is SIGBUS */
    unsigned char *m = mmap(NULL, 3 * PG, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED) return;
    CHECK(probe_bus(m + PG) == 0);
    CHECK(probe_bus(m + 2 * PG + 9) == BUS_ADRERR);
    CHECK(bus_addr == (uintptr_t)(m + 2 * PG + 9));

    /* truncate: pages past the end leave the mapping, the rest of the last
     * page reads as zero */
    unsigned char *pm = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(pm != MAP_FAILED);
    (void)*(volatile unsigned char *)(pm + PG);   /* mapped, not copied */
    CHECK(ftruncate(fd, 100) == 0);
    CHECK(probe_bus(m + PG + 5) == BUS_ADRERR);
    CHECK(bus_addr == (uintptr_t)(m + PG + 5));
    CHECK(probe_bus(pm + PG) == BUS_ADRERR);   /* the private one too */
    CHECK(m[99] == expect_byte(99, 5) && m[100] == 0 && m[PG - 1] == 0);
    CHECK(ftruncate(fd, 2 * PG) == 0);   /* grown again: zeros, no SIGBUS */
    CHECK(probe_bus(m + PG + 5) == 0);
    CHECK(m[PG + 5] == 0);
    munmap(pm, 2 * PG);
    munmap(m, 3 * PG);
    close(fd);
    CHECK(unlink(path) == 0);
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    sigaction(SIGBUS, &sa, NULL);
    if (failures == before) printf("pctest: sigbus ok\n");
}

/* --- fat --- */

static void pc_fat(void) {
    int before = failures;
    static unsigned char whole[16384];
    drop_caches();
    long in0 = vmstat("pgpgin"), rap0 = vmstat("ra_pages"), rab0 = vmstat("ra_batches");
    CHECK(in0 >= 0 && rap0 >= 0 && rab0 >= 0);
    int fd = open("/disk/big.txt", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size > 3 * PG && st.st_size < (long)sizeof(whole));
    long size = st.st_size;
    char c;
    CHECK(read(fd, &c, 1) == 1 && c == '1');
    /* one miss read the whole 4-page file: one readahead batch */
    CHECK(vmstat("pgpgin") >= in0 + 4);
    CHECK(vmstat("ra_pages") >= rap0 + 3);
    CHECK(vmstat("ra_batches") >= rab0 + 1);
    long in1 = vmstat("pgpgin");
    CHECK(pread(fd, whole, sizeof(whole), 0) == size);
    CHECK(vmstat("pgpgin") == in1);   /* all cached now */

    /* a private mapping: the file's bytes, and stores copy */
    unsigned char *pm = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(pm != MAP_FAILED);
    if (pm != MAP_FAILED) {
        CHECK(memcmp(pm, whole, (size_t)size) == 0);
        CHECK(vmstat("pgpgin") == in1);
        pm[0] = 'X';
        CHECK(pread(fd, &c, 1, 0) == 1 && c == '1');
        munmap(pm, (size_t)size);
    }
    /* read-only filesystem, O_RDONLY file: no shared writable mapping */
    errno = 0;
    CHECK(mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EACCES);

    /* WILLNEED reads ahead without a touch */
    unsigned char *sm = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(sm != MAP_FAILED);
    if (sm != MAP_FAILED) {
        drop_caches();
        long in2 = vmstat("pgpgin");
        CHECK(madvise(sm, (size_t)size, MADV_WILLNEED) == 0);
        CHECK(vmstat("pgpgin") >= in2 + 4);
        long in3 = vmstat("pgpgin");
        CHECK(madvise(sm, (size_t)size, MADV_RANDOM) == 0);
        CHECK(madvise(sm, (size_t)size, MADV_SEQUENTIAL) == 0);
        CHECK(madvise(sm, (size_t)size, MADV_NORMAL) == 0);
        CHECK(memcmp(sm, whole, (size_t)size) == 0);
        CHECK(vmstat("pgpgin") == in3);
        munmap(sm, (size_t)size);
    }
    close(fd);
    if (failures == before) printf("pctest: fat ok\n");
}

/* --- big --- */

#define BIG_SIZE  (1L << 30)
#define BIG_WORDS (BIG_SIZE / 8)
#define BIG_CHUNK (1L << 20)
#define BIG_PROCS 4

static void pc_big(void) {
    int before = failures;
    const char *path = "/tmp/pc_big";
    long cached0 = meminfo("Cached");
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    /* word i holds i */
    uint64_t *chunk = malloc(BIG_CHUNK);
    CHECK(chunk != NULL);
    if (chunk == NULL) return;
    for (long c = 0; c < BIG_SIZE / BIG_CHUNK; c++) {
        uint64_t base = (uint64_t)c * (BIG_CHUNK / 8);
        for (long i = 0; i < BIG_CHUNK / 8; i++) chunk[i] = base + (uint64_t)i;
        if (write(fd, chunk, BIG_CHUNK) != BIG_CHUNK) {
            CHECK(!"write of the 1 GiB file");
            break;
        }
    }
    free(chunk);
    CHECK(fsync(fd) == 0);
    long cached1 = meminfo("Cached");
    CHECK(cached1 - cached0 >= BIG_SIZE / 1024);   /* kB */
    long free0 = meminfo("MemFree");

    int ready[2], go[2];
    CHECK(pipe(ready) == 0 && pipe(go) == 0);
    pid_t kids[BIG_PROCS];
    for (int k = 0; k < BIG_PROCS; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            close(ready[0]);
            close(go[1]);
            const uint64_t *m = mmap(NULL, BIG_SIZE, PROT_READ, MAP_SHARED, fd, 0);
            if (m == MAP_FAILED) _exit(1);
            uint64_t sum = 0;
            for (long i = 0; i < BIG_WORDS; i++) sum += m[i];
            const uint64_t want = (uint64_t)BIG_WORDS * (uint64_t)(BIG_WORDS - 1) / 2;
            char r = sum == want ? 'y' : 'n';
            if (write(ready[1], &r, 1) != 1) _exit(2);
            /* stay mapped until the parent has looked at the counters */
            char g;
            (void)read(go[0], &g, 1);
            _exit(r == 'y' ? 0 : 3);
        }
        CHECK(kids[k] > 0);
    }
    close(ready[1]);
    close(go[0]);
    int good = 0;
    for (int k = 0; k < BIG_PROCS; k++) {
        char r = 0;
        if (read(ready[0], &r, 1) == 1 && r == 'y') good++;
    }
    CHECK(good == BIG_PROCS);
    /* four processes map 1 GiB: the pages are counted once, and memory went
     * to page tables (2 MiB each), not to copies */
    long mapped = meminfo("Mapped");
    long free1 = meminfo("MemFree");
    CHECK(mapped >= BIG_SIZE / 1024 && mapped < 2 * (BIG_SIZE / 1024));
    CHECK(free0 - free1 < 64 * 1024);
    printf("pctest: big: 4 processes map 1 GiB: Mapped %ld kB, MemFree dropped %ld kB\n", mapped, free0 - free1);
    close(go[1]);   /* EOF: the children exit */
    for (int k = 0; k < BIG_PROCS; k++) {
        int st = 0;
        CHECK(waitpid(kids[k], &st, 0) == kids[k] && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    close(ready[0]);
    CHECK(meminfo("Mapped") < mapped - BIG_SIZE / 1024 + 1024);
    close(fd);
    CHECK(unlink(path) == 0);
    CHECK(meminfo("Cached") <= cached0 + 1024);   /* the pages went with the file */
    if (failures == before) printf("pctest: big ok\n");
}

int main(int argc, char **argv) {
    int with_disk = argc > 1 && strcmp(argv[1], "fat") == 0;
    pc_coherent();
    pc_private();
    pc_fork();
    pc_sync();
    pc_sigbus();
    if (with_disk) pc_fat();
    pc_big();
    printf("pctest: %d failures\n", failures);
    return failures;
}
