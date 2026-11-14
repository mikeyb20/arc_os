/* arc_os libc — POSIX wrappers over Linux x86_64 syscalls (ADR-0001) */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include "syscall_arch.h"

int errno;

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall_ret(syscall3(__NR_read, (uint64_t)fd, (uint64_t)buf, count));
}

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall_ret(syscall3(__NR_write, (uint64_t)fd, (uint64_t)buf, count));
}

int open(const char *path, int flags, ...) {
    /* mode, the third argument, is read only with O_CREAT (POSIX) */
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return (int)syscall_ret(syscall3(__NR_open, (uint64_t)path, (uint64_t)flags, mode));
}

int close(int fd) {
    return (int)syscall_ret(syscall1(__NR_close, (uint64_t)fd));
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)syscall_ret(syscall3(__NR_lseek, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence));
}

int dup(int fd) {
    return (int)syscall_ret(syscall1(__NR_dup, (uint64_t)fd));
}

int dup2(int oldfd, int newfd) {
    return (int)syscall_ret(syscall2(__NR_dup2, (uint64_t)oldfd, (uint64_t)newfd));
}

int unlink(const char *path) {
    return (int)syscall_ret(syscall1(__NR_unlink, (uint64_t)path));
}

int rename(const char *oldpath, const char *newpath) {
    return (int)syscall_ret(syscall2(__NR_rename, (uint64_t)oldpath, (uint64_t)newpath));
}

int rmdir(const char *path) {
    return (int)syscall_ret(syscall1(__NR_rmdir, (uint64_t)path));
}

int pipe(int pipefd[2]) {
    return (int)syscall_ret(syscall1(__NR_pipe, (uint64_t)pipefd));
}

pid_t fork(void) {
    return (pid_t)syscall_ret(syscall0(__NR_fork));
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)syscall_ret(syscall3(__NR_execve, (uint64_t)path, (uint64_t)argv, (uint64_t)envp));
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

void _exit(int status) {
    syscall1(__NR_exit, (uint64_t)status);
    for (;;) __asm__ volatile ("ud2");
}

void sync(void) {
    (void)syscall0(__NR_sync);
}

pid_t getpid(void) {
    return (pid_t)syscall0(__NR_getpid);
}

pid_t getppid(void) {
    return (pid_t)syscall0(__NR_getppid);
}

uid_t getuid(void) {
    return (uid_t)syscall0(__NR_getuid);
}

gid_t getgid(void) {
    return (gid_t)syscall0(__NR_getgid);
}

int setuid(uid_t uid) {
    return (int)syscall_ret(syscall1(__NR_setuid, (uint64_t)uid));
}

int setgid(gid_t gid) {
    return (int)syscall_ret(syscall1(__NR_setgid, (uint64_t)gid));
}

int setpgid(pid_t pid, pid_t pgid) {
    return (int)syscall_ret(syscall2(__NR_setpgid, (uint64_t)pid, (uint64_t)pgid));
}

pid_t getpgid(pid_t pid) {
    return (pid_t)syscall_ret(syscall1(__NR_getpgid, (uint64_t)pid));
}

int ioctl(int fd, unsigned long request, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, request);
    uint64_t arg = __builtin_va_arg(ap, uint64_t);
    __builtin_va_end(ap);
    return (int)syscall_ret(syscall3(__NR_ioctl, (uint64_t)fd, (uint64_t)request, arg));
}

int tcsetpgrp(int fd, pid_t pgrp) {
    return ioctl(fd, TIOCSPGRP, &pgrp);
}

pid_t tcgetpgrp(int fd) {
    pid_t pgrp;
    if (ioctl(fd, TIOCGPGRP, &pgrp) < 0) return -1;
    return pgrp;
}

int chdir(const char *path) {
    return (int)syscall_ret(syscall1(__NR_chdir, (uint64_t)path));
}

char *getcwd(char *buf, size_t size) {
    /* The kernel returns the length (with the NUL) on success */
    if (syscall_ret(syscall2(__NR_getcwd, (uint64_t)buf, size)) < 0) return NULL;
    return buf;
}

/* sbrk on Linux brk: brk returns the new break, or the old one on failure */
static uintptr_t cur_brk;

void *sbrk(intptr_t increment) {
    if (cur_brk == 0) {
        cur_brk = (uintptr_t)syscall1(__NR_brk, 0);
    }
    if (increment == 0) return (void *)cur_brk;

    uintptr_t old_brk = cur_brk;
    uintptr_t want = old_brk + (uintptr_t)increment;
    if ((increment > 0 && want < old_brk) || (increment < 0 && want > old_brk)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    uintptr_t got = (uintptr_t)syscall1(__NR_brk, (uint64_t)want);
    if (got != want) {
        errno = ENOMEM;
        return (void *)-1;
    }
    cur_brk = got;
    return (void *)old_brk;
}
