/* arc_os — ext2 writing across a reboot (M1-15), under musl. Test ISOs only.
 * Runs in QEMU on the disk tools/make-arc-test-disks.sh make-write builds,
 * booted with arc.data=PARTUUID= naming its data partition (mounted
 * read-write; the kernel marks it clean at the end of the run).
 *
 *   ext2wtest write   (first boot)
 *     mounts   /var and the other data directories are read-write ext2;
 *     big      a 1 GiB patterned file written to /var/big.tmp, fsync'd, then
 *              renamed to /var/big.bin (the temp + fsync + rename pattern);
 *     replace  /var/state replaced atomically the same way;
 *     tree     200 files in /home/user/docs, 100 removed, 20 renamed, a
 *              subdirectory moved; then sync().
 *   ext2wtest verify  (second boot, same disk)
 *     every word of /var/big.bin, no /var/big.tmp, /var/state's new text,
 *     the tree as the first boot left it; then /var/big.bin truncated and
 *     removed (1 GiB freed: the host's e2fsck checks the bitmaps after).
 *
 * The host checks the partition with e2fsck after each boot (--check).
 * Prints one FAIL line per failed check; exit status = number of failures. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("FAIL ext2wtest:%d: %s (errno %d)\n", __LINE__, #cond, errno); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

#define BIG_SIZE (1ll << 30)
#define CHUNK    (1 << 20)

static uint64_t words[CHUNK / 8];

/* The 64-bit word at byte offset 8 * j of the big file */
static uint64_t big_word(uint64_t j) {
    return (j * 0x9E3779B97F4A7C15ull) ^ (j >> 7) ^ 0xA5A5000000000000ull;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static int write_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n > 0) {
        ssize_t w = write(fd, c, n);
        if (w <= 0) return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

/* path gets text by the temp + fsync + rename pattern */
static void replace_file(const char *path, const char *text) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(write_all(fd, text, strlen(text)) == 0);
    CHECK(fsync(fd) == 0);
    CHECK(close(fd) == 0);
    CHECK(rename(tmp, path) == 0);
}

static int file_is(const char *path, const char *text) {
    char buf[256];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    return n == (ssize_t)strlen(text) && memcmp(buf, text, (size_t)n) == 0;
}

static void check_rw_mounts(void) {
    static char m[4096];
    int fd = open("/proc/mounts", O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = fd >= 0 ? read(fd, m, sizeof(m) - 1) : -1;
    if (fd >= 0) close(fd);
    m[n > 0 ? n : 0] = '\0';
    static const char *const dirs[] = {"apps", "models", "packs", "home", "var"};
    for (size_t i = 0; i < 5; i++) {
        char line[64];
        snprintf(line, sizeof(line), "vda2 /%s ext2 rw 0 0\n", dirs[i]);
        CHECK(strstr(m, line) != NULL);
    }
    CHECK(strstr(m, "vda1 /system ext2 ro 0 0\n") != NULL);   /* the system partition stays read-only */
    printf("ext2wtest: mounts ok\n");
}

static void write_big(void) {
    double t0 = now_s();
    int fd = open("/var/big.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    for (long long off = 0; off < BIG_SIZE; off += CHUNK) {
        for (size_t k = 0; k < CHUNK / 8; k++) words[k] = big_word((uint64_t)off / 8 + k);
        if (write_all(fd, words, CHUNK) != 0) {
            CHECK(!"write");
            break;
        }
    }
    double t1 = now_s();
    CHECK(fsync(fd) == 0);
    double t2 = now_s();
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == BIG_SIZE);
    CHECK(st.st_blocks >= BIG_SIZE / 512);   /* every block allocated */
    CHECK(close(fd) == 0);
    CHECK(rename("/var/big.tmp", "/var/big.bin") == 0);
    printf("ext2wtest: wrote %lld bytes (write %.1f s, fsync %.1f s)\n", (long long)BIG_SIZE, t1 - t0, t2 - t1);
}

static void verify_big(void) {
    struct stat st;
    CHECK(stat("/var/big.tmp", &st) != 0 && errno == ENOENT);
    int fd = open("/var/big.bin", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(fstat(fd, &st) == 0 && st.st_size == BIG_SIZE);
    long long bad = 0;
    for (long long off = 0; off < BIG_SIZE; off += CHUNK) {
        ssize_t n = pread(fd, words, CHUNK, off);
        if (n != CHUNK) {
            CHECK(!"short read");
            break;
        }
        for (size_t k = 0; k < CHUNK / 8; k++) bad += words[k] != big_word((uint64_t)off / 8 + k);
    }
    close(fd);
    printf("ext2wtest: verify %lld bytes, %lld bad words\n", (long long)BIG_SIZE, bad);
    CHECK(bad == 0);
}

static void write_tree(void) {
    CHECK(mkdir("/home/user/docs", 0755) == 0);
    CHECK(mkdir("/home/user/docs/sub", 0755) == 0);
    char path[128], text[64];
    for (int i = 0; i < 200; i++) {
        snprintf(path, sizeof(path), "/home/user/docs/note%03d.txt", i);
        snprintf(text, sizeof(text), "note %d\n", i);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        CHECK(fd >= 0 && write_all(fd, text, strlen(text)) == 0);
        if (fd >= 0) close(fd);
    }
    for (int i = 0; i < 200; i += 2) {
        snprintf(path, sizeof(path), "/home/user/docs/note%03d.txt", i);
        CHECK(unlink(path) == 0);
    }
    for (int i = 1; i < 40; i += 2) {
        char to[128];
        snprintf(path, sizeof(path), "/home/user/docs/note%03d.txt", i);
        snprintf(to, sizeof(to), "/home/user/docs/sub/renamed%03d.txt", i);
        CHECK(rename(path, to) == 0);
    }
    CHECK(rename("/home/user/docs/sub", "/home/user/sub") == 0);   /* a directory to another parent */
    printf("ext2wtest: tree ok\n");
}

static int entries_in(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) n += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
    closedir(d);
    return n;
}

static void verify_tree(void) {
    CHECK(entries_in("/home/user/docs") == 80);
    CHECK(entries_in("/home/user/sub") == 20);
    char path[128], text[64];
    for (int i = 41; i < 200; i += 2) {
        snprintf(path, sizeof(path), "/home/user/docs/note%03d.txt", i);
        snprintf(text, sizeof(text), "note %d\n", i);
        CHECK(file_is(path, text));
    }
    for (int i = 1; i < 40; i += 2) {
        snprintf(path, sizeof(path), "/home/user/sub/renamed%03d.txt", i);
        snprintf(text, sizeof(text), "note %d\n", i);
        CHECK(file_is(path, text));
    }
    struct stat st;
    CHECK(stat("/home/user/sub/..", &st) == 0);
    struct stat home;
    CHECK(stat("/home/user", &home) == 0 && st.st_ino == home.st_ino);   /* ".." follows the move */
    CHECK(home.st_nlink == 4);   /* ".", "docs", "sub" and its entry in /home */
    printf("ext2wtest: tree verified\n");
}

int main(int argc, char **argv) {
    int verify = argc > 1 && strcmp(argv[1], "verify") == 0;
    check_rw_mounts();
    if (!verify) {
        replace_file("/var/state", "old state\n");
        write_big();
        replace_file("/var/state", "new state\n");
        CHECK(file_is("/var/state", "new state\n"));
        write_tree();
        sync();
        printf("ext2wtest: write ok\n");
    } else {
        verify_big();
        CHECK(file_is("/var/state", "new state\n"));
        struct stat st;
        CHECK(stat("/var/state.tmp", &st) != 0 && errno == ENOENT);
        verify_tree();
        /* Free the gigabyte: a truncate through every map level, then the inode */
        int fd = open("/var/big.bin", O_WRONLY);
        CHECK(fd >= 0 && ftruncate(fd, 0) == 0);
        if (fd >= 0) close(fd);
        CHECK(unlink("/var/big.bin") == 0);
        sync();
        printf("ext2wtest: freed ok\n");
    }
    printf("ext2wtest: %d failures\n", failures);
    return failures;
}
