/* arc_os libc — wait/waitpid */

#include <sys/wait.h>
#include <syscall.h>
#include <errno.h>

extern int errno;

pid_t waitpid(pid_t pid, int *status, int options) {
    int64_t ret = syscall3(SYS_WAIT, (uint64_t)pid, (uint64_t)status, (uint64_t)options);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)ret;
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}
