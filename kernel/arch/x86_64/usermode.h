#ifndef ARCHOS_ARCH_X86_64_USERMODE_H
#define ARCHOS_ARCH_X86_64_USERMODE_H

#include <stdint.h>

/* Jump to user-mode via IRETQ. Does not return.
 * entry_rip: user-space entry point (RIP)
 * user_rsp:  user-space stack pointer (RSP)
 * argc:      argument count (passed in RDI to user)
 * argv_ptr:  pointer to argv array (passed in RSI to user) */
__attribute__((noreturn))
extern void jump_to_usermode(uint64_t entry_rip, uint64_t user_rsp,
                             uint64_t argc, uint64_t argv_ptr);

/* Forward declaration */
struct ForkContext;

/* Return to user mode as a forked child (RAX=0).
 * ctx: pointer to ForkContext with saved user registers */
__attribute__((noreturn))
extern void fork_return_to_user(const struct ForkContext *ctx);

#endif /* ARCHOS_ARCH_X86_64_USERMODE_H */
