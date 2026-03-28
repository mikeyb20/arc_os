/* arc_os libc — signal/kill wrappers */

#include <signal.h>
#include <syscall.h>
#include <errno.h>

extern int errno;

sig_t signal(int signo, sig_t handler) {
    int64_t ret = syscall2(SYS_SIGNAL, (uint64_t)signo, (uint64_t)handler);
    if (ret < 0) { errno = (int)(-ret); return SIG_DFL; }
    return (sig_t)(uintptr_t)ret;
}

int kill(pid_t pid, int signo) {
    int64_t ret = syscall2(SYS_KILL, (uint64_t)pid, (uint64_t)signo);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}
