#ifndef ARCHOS_ARCH_X86_64_MSR_H
#define ARCHOS_ARCH_X86_64_MSR_H

#include <stdint.h>

/* Model-Specific Register addresses */
#define MSR_EFER    0xC0000080  /* Extended Feature Enable Register */
#define MSR_STAR    0xC0000081  /* Segment selectors for SYSCALL/SYSRET */
#define MSR_LSTAR   0xC0000082  /* SYSCALL entry point (64-bit) */
#define MSR_SFMASK  0xC0000084  /* RFLAGS mask for SYSCALL */
#define MSR_FS_BASE 0xC0000100  /* FS segment base: user TLS pointer (ADR-0002) */
#define MSR_GS_BASE        0xC0000101  /* GS base: this CPU's PerCpu block while in the kernel */
#define MSR_KERNEL_GS_BASE 0xC0000102  /* swapped with GS base by SWAPGS (user GS in the kernel) */
#define MSR_APIC_BASE      0x0000001B  /* IA32_APIC_BASE: enable bits + xAPIC MMIO base */
#define MSR_UCODE_REV      0x0000008B  /* IA32_BIOS_SIGN_ID (Intel: revision in the high half) / AMD patch level */
#define MSR_TSC_DEADLINE   0x000006E0  /* IA32_TSC_DEADLINE: local APIC timer fires at this TSC value; 0 disarms */
#define MSR_X2APIC_FIRST   0x00000800  /* x2APIC register r is MSR 0x800 + (MMIO offset >> 4) */

/* Power (M1-21, ADR-0017): Intel HWP and RAPL. Probe with rdmsr_safe. */
#define MSR_PM_ENABLE              0x00000770  /* IA32_PM_ENABLE: bit 0 turns HWP on (until reset) */
#define MSR_HWP_CAPABILITIES       0x00000771  /* IA32_HWP_CAPABILITIES: highest, guaranteed, efficient, lowest */
#define MSR_HWP_REQUEST            0x00000774  /* IA32_HWP_REQUEST: min, max, desired, EPP (per logical CPU) */
#define MSR_RAPL_POWER_UNIT        0x00000606  /* power, energy and time units */
#define MSR_PKG_ENERGY_STATUS      0x00000611  /* package energy, 32-bit counter */
#define MSR_PP0_ENERGY_STATUS      0x00000639  /* cores (power plane 0) energy, 32-bit counter */
#define MSR_PLATFORM_ENERGY_STATUS 0x0000064D  /* platform (PSYS) energy, 32-bit counter */

/* EFER bits */
#define EFER_SCE    (1ULL << 0) /* System Call Extensions enable */

/* IA32_APIC_BASE bits */
#define APIC_BASE_BSP       (1ULL << 8)    /* this CPU is the bootstrap processor */
#define APIC_BASE_EXTD      (1ULL << 10)   /* x2APIC mode (needs APIC_BASE_EN) */
#define APIC_BASE_EN        (1ULL << 11)   /* local APIC enabled */
#define APIC_BASE_ADDR_MASK 0x000FFFFFFFFFF000ULL

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

/* RDMSR / WRMSR that return -EIO instead of crashing when the CPU raises
 * #GP: the register doesn't exist here or refuses the value (msr_safe.asm,
 * exception-table fixups; M1-21). 0 on success; rdmsr_safe stores 0 in *out
 * on failure. Use them for every register CPUID does not promise. */
int rdmsr_safe(uint32_t msr, uint64_t *out);
int wrmsr_safe(uint32_t msr, uint64_t value);

#endif /* ARCHOS_ARCH_X86_64_MSR_H */
