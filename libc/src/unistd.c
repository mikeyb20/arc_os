/* arc_os libc — POSIX wrappers (syscall invocations) */

#include <unistd.h>
#include <syscall.h>
#include <errno.h>
#include <fcntl.h>

int errno;

static int set_errno(int64_t ret) {
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

ssize_t read(int fd, void *buf, size_t count) {
    int64_t ret = syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, count);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

ssize_t write(int fd, const void *buf, size_t count) {
    int64_t ret = syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, count);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

int open(const char *path, int flags, ...) {
    int64_t ret = syscall3(SYS_OPEN, (uint64_t)path, (uint64_t)flags, 0);
    return set_errno(ret);
}

int close(int fd) {
    return set_errno(syscall1(SYS_CLOSE, (uint64_t)fd));
}

off_t lseek(int fd, off_t offset, int whence) {
    int64_t ret = syscall3(SYS_LSEEK, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (off_t)ret;
}

int dup(int fd) {
    return set_errno(syscall1(SYS_DUP, (uint64_t)fd));
}

int dup2(int oldfd, int newfd) {
    return set_errno(syscall2(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd));
}

int unlink(const char *path) {
    return set_errno(syscall1(SYS_UNLINK, (uint64_t)path));
}

int pipe(int pipefd[2]) {
    return set_errno(syscall1(SYS_PIPE, (uint64_t)pipefd));
}

pid_t fork(void) {
    int64_t ret = syscall0(SYS_FORK);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)ret;
}

int execv(const char *path, char *const argv[]) {
    int64_t ret = syscall2(SYS_EXEC, (uint64_t)path, (uint64_t)argv);
    return set_errno(ret);
}

pid_t getpid(void) {
    return (pid_t)syscall0(SYS_GETPID);
}

pid_t getppid(void) {
    return (pid_t)syscall0(SYS_GETPPID);
}

uid_t getuid(void) {
    return (uid_t)syscall0(SYS_GETUID);
}

gid_t getgid(void) {
    return (gid_t)syscall0(SYS_GETGID);
}

int chdir(const char *path) {
    return set_errno(syscall1(SYS_CHDIR, (uint64_t)path));
}

char *getcwd(char *buf, size_t size) {
    int64_t ret = syscall2(SYS_GETCWD, (uint64_t)buf, size);
    if (ret < 0) { errno = (int)(-ret); return NULL; }
    return buf;
}

/* sbrk via SYS_BRK */
static void *cur_brk;

void *sbrk(intptr_t increment) {
    if (!cur_brk) {
        /* First call: query current brk */
        int64_t ret = syscall1(SYS_BRK, 0);
        if (ret < 0) return (void *)-1;
        cur_brk = (void *)(uintptr_t)ret;
    }
    if (increment == 0) return cur_brk;

    void *old_brk = cur_brk;
    void *new_brk = (void *)((uintptr_t)cur_brk + increment);
    int64_t ret = syscall1(SYS_BRK, (uint64_t)(uintptr_t)new_brk);
    if (ret < 0) return (void *)-1;
    cur_brk = (void *)(uintptr_t)ret;
    return old_brk;
}
