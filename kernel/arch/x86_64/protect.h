#ifndef ARCHOS_ARCH_X86_64_PROTECT_H
#define ARCHOS_ARCH_X86_64_PROTECT_H

#include <stdint.h>

/* CPU memory protection (M1-19, knowledge-os.md §3.2): CR0.WP (the kernel's
 * writes obey read-only pages: W^X for kernel code, copy-on-write for user
 * copies), EFER.NXE (no-execute pages), CR4.SMEP (the kernel can't run user
 * code) and CR4.SMAP (the kernel can't touch user memory unless RFLAGS.AC is
 * set, which only the user-copy routine does, uaccess.asm). Each CPU turns them on for itself
 * (protect_cpu_init); SMEP and SMAP only when the CPU has them: the x86-64-v3
 * floor (Haswell) has SMEP but not SMAP. */

/* protect_cpu_flags() bits */
#define PROTECT_NX   (1u << 0)
#define PROTECT_SMEP (1u << 1)
#define PROTECT_SMAP (1u << 2)
#define PROTECT_WP   (1u << 3)   /* CR0.WP: read-only pages bind the kernel too */

/* RFLAGS.AC: with SMAP on, the kernel may touch user pages only while set */
#define RFLAGS_AC (1ULL << 18)

/* 1 when the CPUs have SMAP, so STAC and CLAC exist: both raise #UD on a CPU
 * without it, so uaccess.asm and the entry stubs test this byte first
 * (smap.inc). Set once on the BSP, before any AP starts; never cleared. */
extern uint8_t protect_smap_on;

/* Turn on WP, NX, SMEP and SMAP on this CPU (each when the CPU has it). Call on
 * every CPU with interrupts off, once the kernel's page tables are loaded
 * (Limine's may map low memory as user pages). */
void protect_cpu_init(void);

/* PROTECT_* bits that are on in this CPU's CR0, EFER and CR4. */
uint32_t protect_cpu_flags(void);

#endif /* ARCHOS_ARCH_X86_64_PROTECT_H */
