#ifndef ARCHOS_ARCH_X86_64_CPUID_H
#define ARCHOS_ARCH_X86_64_CPUID_H

#include <stdint.h>
#include <stddef.h>

/* CPUID and the two values execve needs from it (AT_HWCAP, AT_RANDOM).
 * M1-01 adds the full CPU feature check (kernel/arch/x86_64/cpu.c). */

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(sub));
}

/* AT_HWCAP on x86_64: CPUID leaf 1 EDX (as Linux reports it) */
static inline uint64_t cpu_hwcap(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return d;
}

/* Fill buf with random bytes for AT_RANDOM (musl seeds its stack canary and
 * pointer guard from them). RDRAND when the CPU has it, else TSC mixed through
 * splitmix64: unpredictable enough for a canary, not for keys. M3 crypto
 * needs a real entropy pool. */
static inline void cpu_random_fill(uint8_t *buf, size_t n) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    int have_rdrand = (c >> 30) & 1;
    static uint64_t mix = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = 0;
        int ok = 0;
        for (int tries = 0; have_rdrand && !ok && tries < 10; tries++) {
            uint8_t cf;
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(cf));
            ok = cf;
        }
        if (!ok) {
            uint32_t lo, hi;
            __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
            mix += 0x9E3779B97F4A7C15ULL + (((uint64_t)hi << 32) | lo);
            v = mix;
            v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
            v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
            v ^= v >> 31;
        }
        for (size_t j = 0; j < 8 && i + j < n; j++) buf[i + j] = (uint8_t)(v >> (8 * j));
    }
}

#endif /* ARCHOS_ARCH_X86_64_CPUID_H */
