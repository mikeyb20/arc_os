#ifndef ARCHOS_ARCH_X86_64_SYSCALL_H
#define ARCHOS_ARCH_X86_64_SYSCALL_H

#include <stdint.h>

/* Maximum number of syscalls supported */
#define SYSCALL_MAX 64

/* Syscall numbers */
#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_GETPID  2
#define SYS_OPEN    3
#define SYS_READ    4
#define SYS_CLOSE   5
#define SYS_BRK     6
#define SYS_LSEEK   7
#define SYS_STAT    8
#define SYS_MKDIR   9
#define SYS_READDIR 10
#define SYS_UNLINK  11
#define SYS_FSTAT   12
#define SYS_DUP     13
#define SYS_DUP2    14
#define SYS_GETPPID 15
#define SYS_FORK    16
#define SYS_EXEC    17
#define SYS_WAIT    18
#define SYS_PIPE    19

/* Syscall handler type: up to 6 arguments, returns int64_t */
typedef int64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);

/* Initialize SYSCALL/SYSRET MSRs and register built-in handlers. */
void syscall_init(void);

/* Register a syscall handler for the given number. */
void syscall_register(uint32_t num, syscall_handler_t handler);

/* Assembly entry point for SYSCALL instruction. */
extern void syscall_entry(void);

/* Kernel stack pointer for SYSCALL — updated by scheduler on context switch.
 * Single-CPU only; SMP would need per-CPU storage. */
extern uint64_t syscall_kernel_rsp;

/* C dispatcher called from syscall_entry.asm */
int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5);

#endif /* ARCHOS_ARCH_X86_64_SYSCALL_H */
