#ifndef ARCHOS_ARCH_X86_64_FPU_H
#define ARCHOS_ARCH_X86_64_FPU_H

#include <stdint.h>

/* User FPU/SSE state and the FS base (TLS), switched eagerly per thread.
 *
 * M0-06 stopgap pulled forward from M1-01 (Michael's OK, 2026-11-08): stock musl
 * uses SSE before main, so user SSE is enabled now with a 512-byte FXSAVE area
 * per thread. M1-01 replaces FXSAVE with CPUID-sized XSAVE (AVX) behind the same
 * calls (ADR-0003). The kernel is built with -mgeneral-regs-only, so while a
 * thread is in the kernel its x87/SSE registers still hold its user values. */

#define FPU_STATE_SIZE  512   /* FXSAVE image */
#define FPU_STATE_ALIGN 16

struct Thread;

/* Enable x87/SSE on this CPU (CR0.MP, !CR0.EM, !CR0.TS, CR4.OSFXSR,
 * CR4.OSXMMEXCPT). The BSP call also captures the clean initial image
 * (FCW 0x37F, MXCSR 0x1F80). Call on every CPU before it runs user code. */
void fpu_cpu_init(int is_bsp);

/* Allocate a thread's save area, filled with the clean image. NULL on OOM. */
void *fpu_state_alloc(void);

/* Free a save area from fpu_state_alloc (NULL is fine). */
void fpu_state_free(void *state);

/* Save the live registers into dst (fork: the parent's state for the child). */
void fpu_save_current(void *dst);

/* Load the clean image into the registers (execve). */
void fpu_load_init(void);

/* Context-switch hook, called by the scheduler just before context_switch:
 * saves old's FPU state, loads next's, and loads next's FS base if it differs. */
void arch_thread_switch(struct Thread *old, struct Thread *next);

/* Set the current thread's FS base (arch_prctl ARCH_SET_FS, execve). */
void arch_set_fs_base(uint64_t base);

#endif /* ARCHOS_ARCH_X86_64_FPU_H */
