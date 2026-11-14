/* arc_os — WP, NX, SMEP and SMAP on each CPU (see protect.h, M1-19).
 *
 * Owns: the CR0.WP, EFER.NXE, CR4.SMEP and CR4.SMAP bits, and protect_smap_on.
 * Invariants: every CPU runs with the same protections (the BSP's CPUID
 * decides; hybrid P- and E-cores share one instruction set); RFLAGS.AC is
 * clear in the kernel except inside arch_copy_user (SFMASK clears it on
 * SYSCALL, the entry stubs on every interrupt and exception).
 * Locks: none (each CPU writes its own registers, interrupts off).
 * Cost: a few register writes per CPU at boot; nothing afterwards. */

#include "arch/x86_64/protect.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/msr.h"

#define CPUID7_EBX_SMEP   (1u << 7)
#define CPUID7_EBX_SMAP   (1u << 20)
#define CPUID_EXT1_EDX_NX (1u << 20)

#define CR0_WP   (1ULL << 16)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

uint8_t protect_smap_on;

static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

void protect_cpu_init(void) {
    const CpuFeatures *f = cpu_features();
    /* Without WP a supervisor write ignores read-only pages: kernel code
     * would be writable and a user copy would write through copy-on-write.
     * Limine sets it; this makes it ours. */
    write_cr0(read_cr0() | CR0_WP);
    /* The page tables use the NX bit everywhere (the HHDM, kernel data, user
     * pages); with EFER.NXE off it would be a reserved bit, and the CR3 load
     * before this call would already have faulted. So this write can't be
     * what makes NX work: Limine sets NXE on every CPU it starts, and the
     * floor check (cpu.c) stops a CPU without NX before the VMM runs. It only
     * states that the bit is ours (protect_cpu_flags reports it). */
    if (f->ext1_edx & CPUID_EXT1_EDX_NX) wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    uint64_t cr4 = read_cr4();
    if (f->l7_ebx & CPUID7_EBX_SMEP) cr4 |= CR4_SMEP;
    if (f->l7_ebx & CPUID7_EBX_SMAP) {
        /* AC is clear here (the BSP's RFLAGS from Limine, an AP's from its
         * entry), so turning SMAP on can't leave a user access allowed */
        __atomic_store_n(&protect_smap_on, 1, __ATOMIC_RELAXED);
        cr4 |= CR4_SMAP;
    }
    write_cr4(cr4);
}

uint32_t protect_cpu_flags(void) {
    uint32_t flags = 0;
    if (read_cr0() & CR0_WP) flags |= PROTECT_WP;
    if (rdmsr(MSR_EFER) & EFER_NXE) flags |= PROTECT_NX;
    uint64_t cr4 = read_cr4();
    if (cr4 & CR4_SMEP) flags |= PROTECT_SMEP;
    if (cr4 & CR4_SMAP) flags |= PROTECT_SMAP;
    return flags;
}
