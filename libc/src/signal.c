/* arc_os libc — signal() over rt_sigaction, kill() */

#include <signal.h>
#include <errno.h>
#include "syscall_arch.h"

/* Where a handler returns: the kernel put the rt_sigframe below, and
 * rt_sigreturn restores the interrupted context from it. Linux x86_64 needs
 * every handler to have one (SA_RESTORER), as musl's __restore_rt. */
void __arc_restore_rt(void);
__asm__(".text\n"
        ".global __arc_restore_rt\n"
        ".type __arc_restore_rt, @function\n"
        "__arc_restore_rt:\n"
        "    mov $15, %eax\n"          /* __NR_rt_sigreturn */
        "    syscall\n"
        "    ud2\n");

sig_t signal(int signo, sig_t handler) {
    struct kernel_sigaction act, old;
    act.sa_handler = (uint64_t)(uintptr_t)handler;
    act.sa_flags = SA_RESTORER;
    act.sa_restorer = (uint64_t)(uintptr_t)__arc_restore_rt;
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
