#ifndef ARCHOS_PROC_PROC_SYSCALLS_H
#define ARCHOS_PROC_PROC_SYSCALLS_H

/* Register the process, credential and signal syscalls (exit, getpid, getppid,
 * fork, execve, wait4, kill, rt_sigaction, rt_sigreturn, getuid, getgid, setuid,
 * setgid, setpgid, getpgid) under their Linux numbers. Called by syscall_init. */
void proc_syscalls_init(void);

struct Process;

/* Close every open fd of p (pipe ends drop their refcount, so readers see EOF).
 * Used when a process exits or is killed by a signal. */
void proc_release_fds(struct Process *p);

#endif /* ARCHOS_PROC_PROC_SYSCALLS_H */
