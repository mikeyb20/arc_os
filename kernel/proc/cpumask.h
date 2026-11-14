#ifndef ARCHOS_PROC_CPUMASK_H
#define ARCHOS_PROC_CPUMASK_H

/* A set of CPU indexes (M1-05): thread affinity, P-core and E-core sets.
 * SCHED_MAX_CPUS bits = 32 bytes; sched.c checks it matches HAL_MAX_CPUS. */

#include <stdint.h>

#define SCHED_MAX_CPUS 256
#define CPUMASK_WORDS  (SCHED_MAX_CPUS / 64)

typedef struct CpuMask {
    uint64_t bits[CPUMASK_WORDS];
} CpuMask;

static inline void cpumask_clear(CpuMask *m) {
    for (int i = 0; i < CPUMASK_WORDS; i++) m->bits[i] = 0;
}

static inline void cpumask_fill(CpuMask *m) {
    for (int i = 0; i < CPUMASK_WORDS; i++) m->bits[i] = ~0ULL;
}

static inline void cpumask_set(CpuMask *m, uint32_t cpu) {
    if (cpu < SCHED_MAX_CPUS) m->bits[cpu / 64] |= 1ULL << (cpu % 64);
}

static inline int cpumask_test(const CpuMask *m, uint32_t cpu) {
    return cpu < SCHED_MAX_CPUS && (m->bits[cpu / 64] >> (cpu % 64)) & 1;
}

/* Atomic set / clear of one CPU, for masks other CPUs read without a lock
 * (an mm's "loaded on" set, M1-07). Full barriers. */
static inline void cpumask_set_atomic(CpuMask *m, uint32_t cpu) {
    if (cpu < SCHED_MAX_CPUS) __atomic_fetch_or(&m->bits[cpu / 64], 1ULL << (cpu % 64), __ATOMIC_SEQ_CST);
}

static inline void cpumask_clear_atomic(CpuMask *m, uint32_t cpu) {
    if (cpu < SCHED_MAX_CPUS) __atomic_fetch_and(&m->bits[cpu / 64], ~(1ULL << (cpu % 64)), __ATOMIC_SEQ_CST);
}

/* 1 if a and b share a CPU. */
static inline int cpumask_intersects(const CpuMask *a, const CpuMask *b) {
    for (int i = 0; i < CPUMASK_WORDS; i++) {
        if (a->bits[i] & b->bits[i]) return 1;
    }
    return 0;
}

#endif /* ARCHOS_PROC_CPUMASK_H */
