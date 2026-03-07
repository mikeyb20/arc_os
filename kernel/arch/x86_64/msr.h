#ifndef ARCHOS_ARCH_X86_64_MSR_H
#define ARCHOS_ARCH_X86_64_MSR_H

#include <stdint.h>

/* Model-Specific Register addresses */
#define MSR_EFER    0xC0000080  /* Extended Feature Enable Register */
#define MSR_STAR    0xC0000081  /* Segment selectors for SYSCALL/SYSRET */
#define MSR_LSTAR   0xC0000082  /* SYSCALL entry point (64-bit) */
#define MSR_SFMASK  0xC0000084  /* RFLAGS mask for SYSCALL */

/* EFER bits */
#define EFER_SCE    (1ULL << 0) /* System Call Extensions enable */

/* Read a Model-Specific Register. */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Write a Model-Specific Register. */
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

#endif /* ARCHOS_ARCH_X86_64_MSR_H */
