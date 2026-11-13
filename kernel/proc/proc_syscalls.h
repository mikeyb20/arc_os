#ifndef ARCHOS_PROC_PROC_SYSCALLS_H
#define ARCHOS_PROC_PROC_SYSCALLS_H

/* Register the process, credential and signal syscalls (exit, exit_group,
 * getpid, getppid, fork, execve, wait4, kill, rt_sigaction, rt_sigreturn,
 * getuid, getgid, setuid, setgid, setpgid, getpgid, arch_prctl,
 * set_tid_address) under their Linux numbers. Called by syscall_init. */
void proc_syscalls_init(void);

#include <stdint.h>
#include "proc/elf.h"
#include "proc/exec_stack.h"

struct Process;

/* Enter a freshly loaded program (execve, init): build the System V initial
 * stack from strings and the auxv values, copy it to the top of the user stack
 * of p's live address space, reset the FS base and FPU/SSE state, and jump to
 * the entry point. Takes ownership of strings and stack_buf (both kmalloc'd,
 * stack_buf EXEC_STACK_BUF bytes) and frees them. Does not return. */
__attribute__((noreturn))
void proc_enter_user(struct Process *p, const ElfLoadResult *elf, ExecStrings *strings,
                     const char *execfn, uint8_t *stack_buf);

/* Drop p's fd table (the last owner closes every fd: pipe ends and sockets
 * drop a reference). Used when a process exits or is killed by a signal. */
void proc_release_fds(struct Process *p);

/* End the current process p (exit, exit_group, fatal signal): close its fds,
 * hand its children to init, become a zombie with this status (term_signal
 * nonzero = killed by that signal), wake the parent and send it SIGCHLD, and
 * switch away for good. The parent's wait4 frees everything. Does not return. */
__attribute__((noreturn)) void proc_exit(struct Process *p, int32_t exit_status, uint8_t term_signal);

#endif /* ARCHOS_PROC_PROC_SYSCALLS_H */
