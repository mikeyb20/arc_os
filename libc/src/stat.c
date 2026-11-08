/* arc_os libc — stat/fstat/chmod/chown/mkdir/umask */

#include <sys/stat.h>
#include <errno.h>
#include "syscall_arch.h"

int stat(const char *path, struct stat *buf) {
    return (int)syscall_ret(syscall2(__NR_stat, (uint64_t)path, (uint64_t)buf));
}

int fstat(int fd, struct stat *buf) {
    return (int)syscall_ret(syscall2(__NR_fstat, (uint64_t)fd, (uint64_t)buf));
}

int chmod(const char *path, mode_t mode) {
    return (int)syscall_ret(syscall2(__NR_chmod, (uint64_t)path, (uint64_t)mode));
}

int chown(const char *path, uid_t owner, gid_t group) {
    return (int)syscall_ret(syscall3(__NR_chown, (uint64_t)path, (uint64_t)owner, (uint64_t)group));
}

int mkdir(const char *path, mode_t mode) {
    return (int)syscall_ret(syscall2(__NR_mkdir, (uint64_t)path, (uint64_t)mode));
}

mode_t umask(mode_t mask) {
    return (mode_t)syscall1(__NR_umask, (uint64_t)mask);
}
