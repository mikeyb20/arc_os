/* arc_os — arc_os's own ext2 partitions mounted at boot (M1-13), under musl.
 * Test ISOs only. Runs in QEMU (suites "ext2" and "ext2named") with two
 * virtio disks made by tools/make-arc-test-disks.sh: a foreign disk first
 * (vda: Linux and Windows partitions labelled arc-system and arc-data) and
 * the arc_os disk second (vdb).
 *   mounts    /proc/mounts: /system from vdb1, /apps ... /var from vdb2, all
 *             ext2 read-only, /disk from vdb3 (the arc_os disk's ESP);
 *             nothing from vda (its ESP included);
 *   crc       one "ext2test: crc PATH HEX" line per file (CRC-32), checked
 *             against the host's by the suite's --check;
 *   tree      symlinks, "..", getcwd inside /system, readdir, EROFS;
 *   exec      /system/bin/hello runs from the ext2 page cache;
 *   named     (argument "named": arc.data names the foreign Windows
 *             partition) /system mounted, no data mounts at all, /disk
 *             still the arc_os disk's ESP.
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
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL ext2test:%d: %s (errno %d)\n", __LINE__, #cond, errno); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint8_t buf[1 << 16];

/* CRC-32 of a whole file, printed for the host check */
static void crc_file(const char *path) {
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    uint32_t crc = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) crc = crc_update(crc, buf, (size_t)n);
    CHECK(n == 0);
    close(fd);
    printf("ext2test: crc %s %08x\n", path, crc);
}

static void crc_sparse(void) {
    static const long long offs[] = {0, 49152, 4243456, 1ll << 30, (5ll << 30) - 4096};
    int fd = open("/system/sparse.bin", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 5ll << 30);
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        CHECK(pread(fd, buf, 4096, offs[i]) == 4096);
        printf("ext2test: crc /system/sparse.bin@%lld %08x\n", offs[i], crc_update(0, buf, 4096));
    }
    /* A hole between them reads as zeros */
    memset(buf, 0xAA, 4096);
    CHECK(pread(fd, buf, 4096, 3ll << 30) == 4096);
    int zero = 1;
    for (int i = 0; i < 4096; i++) zero &= buf[i] == 0;
    CHECK(zero);
    CHECK(pread(fd, buf, 4096, 5ll << 30) == 0);
    close(fd);
}

static int mounts_has(const char *text, const char *line) {
    return strstr(text, line) != NULL;
}

static void read_mounts(char *text, size_t size) {
    int fd = open("/proc/mounts", O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = fd >= 0 ? read(fd, text, size - 1) : -1;
    text[n > 0 ? n : 0] = '\0';
    if (fd >= 0) close(fd);
}

static void check_mounts(void) {
    static char m[4096];
    read_mounts(m, sizeof(m));
    CHECK(mounts_has(m, "vdb1 /system ext2 ro 0 0\n"));
    static const char *const dirs[] = {"apps", "models", "packs", "home", "var"};
    for (size_t i = 0; i < 5; i++) {
        char line[64];
        snprintf(line, sizeof(line), "vdb2 /%s ext2 ro 0 0\n", dirs[i]);
        CHECK(mounts_has(m, line));
    }
    CHECK(mounts_has(m, "vdb3 /disk vfat "));   /* the arc_os disk's ESP */
    struct stat st;
    CHECK(stat("/disk/ARC.TXT", &st) == 0);
    CHECK(!mounts_has(m, "vda"));   /* the foreign disk: nothing, not even its ESP */
    printf("ext2test: mounts ok\n");
}

static void test_tree(void) {
    char text[64];
    int fd = open("/system/link", O_RDONLY);
    CHECK(fd >= 0 && read(fd, text, sizeof(text)) == 5 && memcmp(text, "deep\n", 5) == 0);
    if (fd >= 0) close(fd);
    ssize_t n = readlink("/system/link", text, sizeof(text));
    CHECK(n == 16 && memcmp(text, "dir/sub/deep.txt", 16) == 0);
    /* bufsiz is an int (Linux): the upper 32 bits play no part */
    memset(text, 'x', sizeof(text));
    CHECK(syscall(SYS_readlink, "/system/link", text, (size_t)0x100000005ull) == 5 && text[5] == 'x');
    struct stat st;
    CHECK(lstat("/system/link", &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(chdir("/system/dir/sub") == 0);
    CHECK(getcwd(text, sizeof(text)) != NULL && strcmp(text, "/system/dir/sub") == 0);
    CHECK(stat("../../hello.txt", &st) == 0 && st.st_size == 39);
    CHECK(stat("../../../proc", &st) == 0 && S_ISDIR(st.st_mode));   /* up out of the volume */
    CHECK(chdir("/") == 0);
    /* The data root itself is not mounted, only its five directories */
    CHECK(stat("/not-mounted.txt", &st) == -1 && errno == ENOENT);
    CHECK(stat("/home/user/notes.txt", &st) == 0);
    /* Read-only */
    errno = 0;
    CHECK(open("/system/new", O_CREAT | O_WRONLY, 0644) == -1 && errno == EROFS);
    CHECK(open("/system/hello.txt", O_RDWR) == -1 && errno == EROFS);
    CHECK(mkdir("/apps/x", 0755) == -1 && errno == EROFS);
    CHECK(unlink("/var/log.txt") == -1 && errno == EROFS);
    /* readdir */
    DIR *d = opendir("/system");
    CHECK(d != NULL);
    int seen = 0;
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, "big.bin") == 0 && e->d_type == DT_REG) seen |= 1;
        if (strcmp(e->d_name, "dir") == 0 && e->d_type == DT_DIR) seen |= 2;
        if (strcmp(e->d_name, "link") == 0 && e->d_type == DT_LNK) seen |= 4;
    }
    if (d != NULL) closedir(d);
    CHECK(seen == 7);
    printf("ext2test: tree ok\n");
}

static void test_exec(void) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/system/bin/hello", "hello", "ext2", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("ext2test: exec ok\n");
}

static void test_named(void) {
    static char m[4096];
    read_mounts(m, sizeof(m));
    CHECK(mounts_has(m, "vdb1 /system ext2 ro 0 0\n"));
    CHECK(!mounts_has(m, " /apps "));
    CHECK(!mounts_has(m, "vda"));
    CHECK(mounts_has(m, "vdb3 /disk vfat "));
    struct stat st;
    CHECK(stat("/apps/FOREIGN.txt", &st) == -1);
    printf("ext2test: named ok\n");
}

int main(int argc, char **argv) {
    crc_init();
    if (argc > 1 && strcmp(argv[1], "named") == 0) {
        test_named();
    } else {
        check_mounts();
        crc_file("/system/hello.txt");
        crc_file("/system/dir/sub/deep.txt");
        crc_file("/system/big.bin");
        crc_sparse();
        crc_file("/apps/app.txt");
        crc_file("/models/model.txt");
        crc_file("/packs/pack.bin");
        crc_file("/home/user/notes.txt");
        crc_file("/var/log.txt");
        printf("ext2test: crc ok\n");
        test_tree();
        test_exec();
    }
    printf("ext2test: %d failures\n", failures);
    return failures;
}
