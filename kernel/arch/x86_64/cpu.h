#ifndef ARCHOS_ARCH_X86_64_CPU_H
#define ARCHOS_ARCH_X86_64_CPU_H

#include <stdint.h>
#include <stddef.h>

/* CPU features, the x86-64-v3 floor and hybrid core types (M1-01, ADR-0003).
 *
 * cpu_features_init() reads CPUID once on the BSP; kmain checks the floor right
 * after it (cpu_floor_missing), before anything uses XSAVE. Every CPU then records its own core
 * type (cpu_core_type_detect) because P- and E-cores answer CPUID 0x1A
 * differently. */

/* CPUID words the kernel uses, read once on the BSP */
typedef struct {
    uint32_t max_leaf;       /* CPUID 0 EAX */
    uint32_t max_ext_leaf;   /* CPUID 0x80000000 EAX */
    uint32_t l1_ecx;         /* leaf 1 */
    uint32_t l1_edx;
    uint32_t l7_ebx;         /* leaf 7, subleaf 0 */
    uint32_t l7_ecx;
    uint32_t l7_edx;
    uint32_t ext1_ecx;       /* leaf 0x80000001 */
    uint32_t ext1_edx;
    uint32_t ext7_edx;       /* leaf 0x80000007 (invariant TSC etc.) */
    char vendor[13];     /* "GenuineIntel", "AuthenticAMD", ... */
} CpuFeatures;

/* Core types from CPUID 0x1A EAX[31:24] (hybrid parts only) */
#define CPU_CORE_UNKNOWN 0x00   /* not a hybrid part, or no leaf 0x1A */
#define CPU_CORE_ATOM    0x20   /* E-core */
#define CPU_CORE_CORE    0x40   /* P-core */

/* Read the CPUID leaves above on this CPU (the BSP). Call once, early. */
void cpu_features_init(void);

/* The features read by cpu_features_init. */
const CpuFeatures *cpu_features(void);

/* Write the names of the x86-64-v3 features this CPU lacks into buf, space
 * separated (empty string when none). Returns how many are missing. */
int cpu_floor_missing(char *buf, size_t size);

/* Core type of the CPU this runs on (CPU_CORE_*), from CPUID 0x1A when CPUID 7
 * reports a hybrid part. Call on each CPU and store the result per CPU. */
uint8_t cpu_core_type_detect(void);

/* Short name of a core type: "P", "E" or "-". */
const char *cpu_core_type_name(uint8_t type);

/* Maximum CPUs supported (percpu.h uses the same value) */
#define CPU_MAX 16

/* Describe CPU number cpu (0 .. CPU_MAX - 1) for /proc/cpuinfo: its APIC ID and
 * CPU_CORE_* type. Returns 0, or -1 when that CPU is not online. */
int cpu_describe(uint32_t cpu, uint32_t *apic_id, uint8_t *core_type);

/* AT_HWCAP2 for user programs: Linux's x86 bits (RING3MWAIT, FSGSBASE). arc_os
 * enables neither for user space, so this is 0. */
uint64_t cpu_hwcap2(void);

#endif /* ARCHOS_ARCH_X86_64_CPU_H */
