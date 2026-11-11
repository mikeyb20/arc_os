#ifndef ARCHOS_ARCH_X86_64_SYSCALL_H
#define ARCHOS_ARCH_X86_64_SYSCALL_H

#include <stdint.h>
#include "uapi/unistd.h"

/* Syscalls use the Linux x86_64 numbers in uapi/unistd.h (ADR-0001). The
 * dispatch table has ARC_NR_SYSCALLS slots; an empty slot or a number past the
 * end returns -ENOSYS. Handlers live in their subsystem (fs/, proc/, mm/, net/)
 * and register themselves from syscall_init. */

/* Syscall handler type: up to 6 arguments, returns int64_t (-errno on error) */
typedef int64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);

/* Initialize this CPU's SYSCALL/SYSRET MSRs (BSP and every AP). */
void syscall_cpu_init(void);

/* syscall_cpu_init on the BSP, then register every subsystem's handlers. */
void syscall_init(void);

/* Register a syscall handler for the given Linux number (ignored if out of range). */
void syscall_register(uint32_t num, syscall_handler_t handler);

/* Assembly entry point for SYSCALL instruction. */
extern void syscall_entry(void);

/* C dispatcher called from syscall_entry.asm */
int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5);

/* Copy the calling thread's saved user registers (RIP, RSP, RFLAGS and the
 * callee-saved set) into a fork context, from the SyscallFrame at the top of
 * its kernel stack. Valid only inside a syscall. */
struct ForkContext;
void syscall_save_user_context(struct ForkContext *ctx);

/* The caller's user RSP at SYSCALL entry. Valid only inside a syscall. */
uint64_t syscall_user_rsp(void);

#endif /* ARCHOS_ARCH_X86_64_SYSCALL_H */
