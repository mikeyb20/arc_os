#ifndef ARCHOS_ARCH_X86_64_USERMODE_H
#define ARCHOS_ARCH_X86_64_USERMODE_H

#include <stdint.h>

/* Jump to user-mode via IRETQ. Does not return.
 * entry_rip: user-space entry point (RIP)
 * user_rsp:  user-space stack pointer (RSP) */
__attribute__((noreturn))
extern void jump_to_usermode(uint64_t entry_rip, uint64_t user_rsp);

#endif /* ARCHOS_ARCH_X86_64_USERMODE_H */
