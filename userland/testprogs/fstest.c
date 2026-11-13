/* arc_os — files and directories through the M1-11 VFS, under musl. Test ISOs only.
 *
 * Built with stock musl (static), so the calls are the ones musl makes
 * (openat, newfstatat, getdents64, pread64, ...). Runs in QEMU (suite "vfs"):
 *   open      O_CREAT/O_EXCL/O_TRUNC/O_APPEND/O_DIRECTORY/O_CLOEXEC, mode and umask;
 *   big       pwrite/pread/lseek past 4 GiB on a sparse ramfs file, ftruncate;
 *   share     one offset shared after dup and after fork;
 *   fcntl     F_GETFD/F_SETFD/F_GETFL/F_SETFL/F_DUPFD/F_DUPFD_CLOEXEC, dup3, pipe2;
 *   dirs      mkdirat, renameat across directories, rename over a file,
 *             unlinkat (AT_REMOVEDIR), getdents64 over 300 entries, stat/lstat;
 *   perm      a non-root child is refused a 0600 file and a non-executable exec;
 *   cloexec   (argument "cloexec FD") exec'd with an O_CLOEXEC fd: it is gone;
 *   mounts    /proc/mounts lists /, /dev, /proc (and /disk when a disk is there);
 *   fat       (argument "fat") the FAT32 test disk at /disk: reads, EROFS on writes.
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

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL fstest:%d: %s (errno %d)\n", __LINE__, #cond, errno); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define GIB (1024LL * 1024 * 1024)

static void put_file(const char *path, const char *s, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(write(fd, s, strlen(s)) == (ssize_t)strlen(s));
    close(fd);
}

static ssize_t get_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    return n;
}

static void test_open(void) {
    const char *p = "/tmp/fst_open";
    unlink(p);
    umask(022);
    int fd = open(p, O_RDWR | O_CREAT | O_EXCL, 0666);
    CHECK(fd >= 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & 07777) == 0644 && S_ISREG(st.st_mode));
    CHECK(st.st_uid == 0 && st.st_nlink == 1 && st.st_dev != 0 && st.st_ino != 0);
    CHECK(write(fd, "hello world", 11) == 11);
    close(fd);
    errno = 0;
    CHECK(open(p, O_RDWR | O_CREAT | O_EXCL, 0666) == -1 && errno == EEXIST);
    errno = 0;
    CHECK(open(p, O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
    errno = 0;
    CHECK(open("/tmp/fst_nope/x", O_RDONLY) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(open("/tmp", O_WRONLY) == -1 && errno == EISDIR);

    /* O_APPEND writes land at the end whatever the offset */
    fd = open(p, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, "!", 1) == 1);
    CHECK(lseek(fd, 0, SEEK_CUR) == 12);
    close(fd);
    char buf[64];
    CHECK(get_file(p, buf, sizeof(buf)) == 12 && strcmp(buf, "hello world!") == 0);

    /* O_TRUNC empties it */
    fd = open(p, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 0);
    close(fd);

    /* A directory opens with O_DIRECTORY; read() on it is EISDIR */
    fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(read(fd, buf, sizeof(buf)) == -1 && errno == EISDIR);
    close(fd);
    CHECK(unlink(p) == 0);
    printf("fstest: open ok\n");
}

static void test_big(void) {
    const char *p = "/tmp/fst_big";
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    const off_t far = 5 * GIB + 123;
    CHECK(pwrite(fd, "far", 3, far) == 3);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == far + 3);
    CHECK(st.st_blocks <= 16);   /* sparse: one page, not 5 GiB */
    CHECK(lseek(fd, 0, SEEK_END) == far + 3);
    CHECK(lseek(fd, -3, SEEK_END) == far);
    char buf[8] = {0};
    CHECK(read(fd, buf, sizeof(buf)) == 3 && memcmp(buf, "far", 3) == 0);
    memset(buf, 'x', sizeof(buf));
    CHECK(pread(fd, buf, 4, 4 * GIB + 1) == 4 && memcmp(buf, "\0\0\0\0", 4) == 0);   /* a hole */
    CHECK(pread(fd, buf, 8, far + 3) == 0);                                              /* EOF */
    CHECK(lseek(fd, 0, SEEK_CUR) == far + 3);   /* pread leaves the offset alone */
    errno = 0;
    CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(pread(fd, buf, 1, -1) == -1 && errno == EINVAL);

    /* ftruncate shrinks below 4 GiB and grows again with zeros */
    CHECK(ftruncate(fd, 2) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 2);
    CHECK(ftruncate(fd, 6 * GIB) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 6 * GIB);
    CHECK(pread(fd, buf, 3, far) == 3 && memcmp(buf, "\0\0\0", 3) == 0);
    close(fd);
    CHECK(unlink(p) == 0);
    printf("fstest: big ok\n");
}

static void test_share(void) {
    const char *p = "/tmp/fst_share";
    put_file(p, "abcdefgh", 0644);
    int fd = open(p, O_RDONLY);
    CHECK(fd >= 0);
    int fd2 = dup(fd);
    CHECK(fd2 > fd);
    char c;
    CHECK(read(fd, &c, 1) == 1 && c == 'a');
    CHECK(read(fd2, &c, 1) == 1 && c == 'b');   /* dup shares the offset */
    pid_t pid = fork();
    if (pid == 0) {
        char d;
        _exit(read(fd, &d, 1) == 1 && d == 'c' ? 0 : 1);
    }
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(read(fd2, &c, 1) == 1 && c == 'd');   /* so does fork */
    CHECK(lseek(fd, 0, SEEK_CUR) == 4);
    close(fd);
    CHECK(read(fd2, &c, 1) == 1 && c == 'e');   /* the file outlives one of its fds */
    close(fd2);

    /* Two opens are two files with their own offsets */
    int a = open(p, O_RDONLY), b = open(p, O_RDONLY);
    CHECK(a >= 0 && b >= 0);
    CHECK(read(a, &c, 1) == 1 && c == 'a');
    CHECK(read(b, &c, 1) == 1 && c == 'a');
    close(a);
    close(b);
    CHECK(unlink(p) == 0);
    printf("fstest: share ok\n");
}

static void test_fcntl(void) {
    const char *p = "/tmp/fst_fcntl";
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK(fcntl(fd, F_SETFD, 0) == 0 && fcntl(fd, F_GETFD) == 0);
    CHECK((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR);
    CHECK(fcntl(fd, F_SETFL, O_APPEND | O_NONBLOCK) == 0);
    int fl = fcntl(fd, F_GETFL);
    CHECK((fl & O_APPEND) && (fl & O_NONBLOCK) && (fl & O_ACCMODE) == O_RDWR);
    CHECK(fcntl(fd, F_SETFL, O_RDONLY) == 0 && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR);   /* mode stays */

    int d = fcntl(fd, F_DUPFD, 20);
    CHECK(d == 20 && fcntl(d, F_GETFD) == 0);
    int e = fcntl(fd, F_DUPFD_CLOEXEC, 20);
    CHECK(e == 21 && fcntl(e, F_GETFD) == FD_CLOEXEC);
    errno = 0;
    CHECK(fcntl(fd, F_DUPFD, -1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(fcntl(999, F_GETFD) == -1 && errno == EBADF);
    close(d);
    close(e);

    CHECK(dup3(fd, 30, O_CLOEXEC) == 30 && fcntl(30, F_GETFD) == FD_CLOEXEC);
    errno = 0;
    CHECK(dup3(fd, fd, 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(dup3(fd, 31, O_APPEND) == -1 && errno == EINVAL);
    CHECK(dup2(fd, 30) == 30 && fcntl(30, F_GETFD) == 0);   /* dup2 over it clears cloexec */
    close(30);
    close(fd);

    int pfd[2];
    CHECK(pipe2(pfd, O_NONBLOCK | O_CLOEXEC) == 0);
    CHECK(fcntl(pfd[0], F_GETFD) == FD_CLOEXEC && fcntl(pfd[1], F_GETFD) == FD_CLOEXEC);
    char c;
    errno = 0;
    CHECK(read(pfd[0], &c, 1) == -1 && errno == EAGAIN);
    CHECK(write(pfd[1], "z", 1) == 1 && read(pfd[0], &c, 1) == 1 && c == 'z');
    close(pfd[1]);
    CHECK(read(pfd[0], &c, 1) == 0);   /* EOF once the writer is gone */
    close(pfd[0]);
    errno = 0;
    CHECK(pipe2(pfd, O_APPEND) == -1 && errno == EINVAL);
    CHECK(unlink(p) == 0);
    printf("fstest: fcntl ok\n");
}

static int count_entries(const char *path, int *dots) {
    DIR *d = opendir(path);
    if (d == NULL) return -1;
    int n = 0;
    *dots = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            (*dots)++;
        else
            n++;
    }
    closedir(d);
    return n;
}

static void test_dirs(void) {
    int tmp = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK(tmp >= 0);
    CHECK(mkdirat(tmp, "fst_a", 0755) == 0);
    CHECK(mkdirat(tmp, "fst_b", 0755) == 0);
    errno = 0;
    CHECK(mkdirat(tmp, "fst_a", 0755) == -1 && errno == EEXIST);
    int a = openat(tmp, "fst_a", O_RDONLY | O_DIRECTORY);
    int b = openat(tmp, "fst_b", O_RDONLY | O_DIRECTORY);
    CHECK(a >= 0 && b >= 0);
    int fd = openat(a, "f", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "moved", 5) == 5);

    struct stat st, st2;
    CHECK(fstatat(a, "f", &st, 0) == 0 && st.st_size == 5);
    CHECK(stat("/tmp/fst_a", &st2) == 0 && S_ISDIR(st2.st_mode) && st2.st_nlink == 2);

    /* rename across directories keeps the inode and the open file */
    CHECK(renameat(a, "f", b, "g") == 0);
    errno = 0;
    CHECK(fstatat(a, "f", &st2, 0) == -1 && errno == ENOENT);
    CHECK(fstatat(b, "g", &st2, 0) == 0 && st2.st_ino == st.st_ino);
    CHECK(write(fd, "!", 1) == 1);
    close(fd);
    char buf[16];
    CHECK(get_file("/tmp/fst_b/g", buf, sizeof(buf)) == 6 && strcmp(buf, "moved!") == 0);

    /* rename over an existing file replaces it; a dir into its own subtree fails */
    put_file("/tmp/fst_b/h", "old", 0644);
    CHECK(rename("/tmp/fst_b/g", "/tmp/fst_b/h") == 0);
    CHECK(get_file("/tmp/fst_b/h", buf, sizeof(buf)) == 6);
    CHECK(mkdir("/tmp/fst_a/sub", 0755) == 0);
    errno = 0;
    CHECK(rename("/tmp/fst_a", "/tmp/fst_a/sub/x") == -1 && errno == EINVAL);
    CHECK(rename("/tmp/fst_a/sub", "/tmp/fst_b/sub") == 0);
    CHECK(stat("/tmp/fst_a", &st2) == 0 && st2.st_nlink == 2);
    CHECK(stat("/tmp/fst_b", &st2) == 0 && st2.st_nlink == 3);

    /* unlinkat: a file, then a directory only with AT_REMOVEDIR */
    errno = 0;
    CHECK(unlinkat(b, "sub", 0) == -1 && errno == EISDIR);
    errno = 0;
    CHECK(unlinkat(b, "h", AT_REMOVEDIR) == -1 && errno == ENOTDIR);
    CHECK(unlinkat(b, "h", 0) == 0);
    errno = 0;
    CHECK(rmdir("/tmp/fst_b") == -1 && errno == ENOTEMPTY);
    CHECK(unlinkat(b, "sub", AT_REMOVEDIR) == 0);

    /* getdents64 past the old 256-entry limit */
    char name[32];
    for (int i = 0; i < 300; i++) {
        snprintf(name, sizeof(name), "e%03d", i);
        int f = openat(a, name, O_WRONLY | O_CREAT | O_EXCL, 0644);
        CHECK(f >= 0);
        close(f);
    }
    int dots = 0;
    CHECK(count_entries("/tmp/fst_a", &dots) == 300 && dots == 2);
    for (int i = 0; i < 300; i++) {
        snprintf(name, sizeof(name), "e%03d", i);
        CHECK(unlinkat(a, name, 0) == 0);
    }
    CHECK(count_entries("/tmp/fst_a", &dots) == 0);

    /* stat and lstat through their own syscall numbers (musl uses newfstatat) */
    struct stat k;
    CHECK(syscall(SYS_stat, "/tmp/fst_a", &k) == 0 && S_ISDIR(k.st_mode));
    CHECK(syscall(SYS_lstat, "/tmp/fst_a", &k) == 0 && S_ISDIR(k.st_mode));
    errno = 0;
    CHECK(syscall(SYS_lstat, "/tmp/fst_nope", &k) == -1 && errno == ENOENT);
    CHECK(fstatat(a, "", &k, AT_EMPTY_PATH) == 0 && S_ISDIR(k.st_mode));

    close(a);
    close(b);
    CHECK(unlinkat(tmp, "fst_a", AT_REMOVEDIR) == 0);
    CHECK(unlinkat(tmp, "fst_b", AT_REMOVEDIR) == 0);
    close(tmp);
    printf("fstest: dirs ok\n");
}

/* A non-root child: refused a 0600 root file, and exec of a file without x */
static void test_perm(void) {
    put_file("/tmp/fst_secret", "s", 0600);
    put_file("/tmp/fst_noexec", "#!x\n", 0644);
    pid_t pid = fork();
    if (pid == 0) {
        int bad = 0;
        if (setuid(1000) != 0) _exit(100);
        errno = 0;
        if (!(open("/tmp/fst_secret", O_RDONLY) == -1 && errno == EACCES)) bad |= 1;
        errno = 0;
        if (!(unlink("/tmp/fst_secret") == -1 && errno == EPERM)) bad |= 2;   /* sticky /tmp */
        char *argv[] = {"x", NULL};
        errno = 0;
        if (!(execve("/tmp/fst_noexec", argv, NULL) == -1 && errno == EACCES)) bad |= 4;
        _exit(bad);
    }
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    if (WEXITSTATUS(status) != 0) printf("FAIL fstest: perm child status %d\n", WEXITSTATUS(status));
    failures += WEXITSTATUS(status) != 0;
    CHECK(unlink("/tmp/fst_secret") == 0 && unlink("/tmp/fst_noexec") == 0);
    printf("fstest: perm ok\n");
}

/* exec ourselves with a close-on-exec fd and a plain one: only the plain one is left */
static void test_cloexec(const char *self) {
    int keep = open("/tmp", O_RDONLY | O_DIRECTORY);
    int gone = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(keep >= 0 && gone >= 0);
    pid_t pid = fork();
    if (pid == 0) {
        char a[16], b[16];
        snprintf(a, sizeof(a), "%d", keep);
        snprintf(b, sizeof(b), "%d", gone);
        char *argv[] = {(char *)self, "cloexec", a, b, NULL};
        execve(self, argv, NULL);
        _exit(99);
    }
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(keep);
    close(gone);
    printf("fstest: cloexec ok\n");
}

static int cloexec_child(int keep, int gone) {
    errno = 0;
    int ok = fcntl(keep, F_GETFD) == 0 && fcntl(gone, F_GETFD) == -1 && errno == EBADF;
    return ok ? 0 : 1;
}

static void test_mounts(int with_disk) {
    char buf[1024];
    CHECK(get_file("/proc/mounts", buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, " / ramfs rw") != NULL);
    CHECK(strstr(buf, " /dev devfs rw") != NULL);
    CHECK(strstr(buf, " /proc proc rw") != NULL);
    if (with_disk) CHECK(strstr(buf, "vda /disk vfat ro") != NULL);
    printf("fstest: mounts ok\n");
}

/* The FAT32 test disk (tools/make-fat32-test-disk.sh), mounted read-only at /disk */
static void test_fat(void) {
    char buf[64];
    CHECK(get_file("/disk/hello.txt", buf, sizeof(buf)) == 13 && strcmp(buf, "Hello, FAT32!") == 0);
    CHECK(get_file("/disk/subdir/nested.txt", buf, sizeof(buf)) == 10 && strcmp(buf, "1234567890") == 0);
    CHECK(get_file("/disk/HELLO.TXT", buf, sizeof(buf)) == 13);   /* 8.3 names match in any case */

    /* big.txt is `seq 1 3000` over several clusters: check the last line and a pread */
    int fd = open("/disk/big.txt", O_RDONLY);
    CHECK(fd >= 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 13893 && S_ISREG(st.st_mode));
    CHECK(pread(fd, buf, 5, st.st_size - 5) == 5 && memcmp(buf, "3000\n", 5) == 0);
    long sum = 0;
    char chunk[700];
    ssize_t n;
    long total = 0;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (ssize_t i = 0; i < n; i++) sum += (unsigned char)chunk[i];
        total += n;
    }
    CHECK(n == 0 && total == st.st_size && sum > 0);
    close(fd);

    CHECK(stat("/disk/ro.txt", &st) == 0 && (st.st_mode & 0222) == 0);
    CHECK(stat("/disk/subdir", &st) == 0 && S_ISDIR(st.st_mode));
    int dots = 0;
    CHECK(count_entries("/disk", &dots) == 4);   /* subdir hello.txt big.txt ro.txt */

    /* Read-only: every change is EROFS */
    errno = 0;
    CHECK(open("/disk/hello.txt", O_WRONLY) == -1 && errno == EROFS);
    errno = 0;
    CHECK(open("/disk/new.txt", O_WRONLY | O_CREAT, 0644) == -1 && errno == EROFS);
    errno = 0;
    CHECK(mkdir("/disk/d", 0755) == -1 && errno == EROFS);
    errno = 0;
    CHECK(unlink("/disk/hello.txt") == -1 && errno == EROFS);
    errno = 0;
    CHECK(rename("/disk/hello.txt", "/disk/x.txt") == -1 && errno == EROFS);
    errno = 0;
    CHECK(chmod("/disk/hello.txt", 0600) == -1 && errno == EROFS);
    errno = 0;
    CHECK(open("/disk/nope.txt", O_RDONLY) == -1 && errno == ENOENT);
    printf("fstest: fat ok\n");
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "cloexec") == 0) return cloexec_child(atoi(argv[2]), atoi(argv[3]));
    int with_disk = argc > 1 && strcmp(argv[1], "fat") == 0;
    test_open();
    test_big();
    test_share();
    test_fcntl();
    test_dirs();
    test_perm();
    test_cloexec(argv[0][0] == '/' ? argv[0] : "/boot/fstest");
    test_mounts(with_disk);
    if (with_disk) test_fat();
    printf("fstest: %d failures\n", failures);
    return failures;
}
