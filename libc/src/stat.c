/* arc_os libc — stat/fstat/chmod/chown/mkdir */

#include <sys/stat.h>
#include <syscall.h>
#include <errno.h>

extern int errno;

int stat(const char *path, struct stat *buf) {
    int64_t ret = syscall2(SYS_STAT, (uint64_t)path, (uint64_t)buf);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int fstat(int fd, struct stat *buf) {
    int64_t ret = syscall2(SYS_FSTAT, (uint64_t)fd, (uint64_t)buf);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int chmod(const char *path, mode_t mode) {
    int64_t ret = syscall2(SYS_CHMOD, (uint64_t)path, (uint64_t)mode);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int chown(const char *path, uid_t owner, gid_t group) {
    int64_t ret = syscall3(SYS_CHOWN, (uint64_t)path, (uint64_t)owner, (uint64_t)group);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    int64_t ret = syscall2(SYS_MKDIR, (uint64_t)path, (uint64_t)mode);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}
