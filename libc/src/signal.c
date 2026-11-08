/* arc_os libc — signal() over rt_sigaction, kill() */

#include <signal.h>
#include <errno.h>
#include "syscall_arch.h"

sig_t signal(int signo, sig_t handler) {
    /* Field by field, not "= { 0 }": GCC zeroes structs with SSE stores, and
     * user-mode SSE is not enabled until M1-01. */
    struct kernel_sigaction act, old;
    act.sa_handler = (uint64_t)(uintptr_t)handler;
    act.sa_flags = 0;
    act.sa_restorer = 0;
    act.sa_mask = 0;
    old.sa_handler = 0;
    int64_t ret = syscall4(__NR_rt_sigaction, (uint64_t)signo, (uint64_t)&act,
                           (uint64_t)&old, ARC_SIGSET_SIZE);
    if (syscall_ret(ret) < 0) return SIG_ERR;
    return (sig_t)(uintptr_t)old.sa_handler;
}

int kill(pid_t pid, int signo) {
    return (int)syscall_ret(syscall2(__NR_kill, (uint64_t)pid, (uint64_t)signo));
}
