#ifndef ARCHOS_ARCH_X86_64_FPU_H
#define ARCHOS_ARCH_X86_64_FPU_H

#include <stdint.h>
#include <stddef.h>

/* User FPU/SSE/AVX state and the FS base (TLS), switched eagerly per thread
 * (ADR-0003, M1-01).
 *
 * Each thread has a 64-byte aligned XSAVE area, sized from CPUID 0xD after XCR0
 * is set: x87 | SSE | AVX, plus the AVX-512 components when the CPU has them,
 * never AMX. The kernel is built with -mgeneral-regs-only, so while a thread is
 * in the kernel its vector registers still hold its user values: they are
 * saved only at a context switch (XSAVEOPT, else XSAVE) and loaded with XRSTOR.
 * Signal delivery writes the image to the user stack in Linux's format and
 * rt_sigreturn loads it back after checking it. */

#define FPU_STATE_ALIGN 64

struct Thread;

/* Enable x87/SSE/AVX on this CPU: CR0.MP and NE set, EM and TS clear,
 * CR4.OSFXSR, OSXMMEXCPT and OSXSAVE, XCR0. The BSP call (is_bsp = 1) also
 * sizes the save area and builds the clean image (FCW 0x37F, MXCSR 0x1F80).
 * Needs the x86-64-v3 floor (cpu_floor_missing). Call on every CPU before it
 * runs user code; the BSP first. */
void fpu_cpu_init(int is_bsp);

/* Bytes in one XSAVE image (CPUID 0xD EBX with this XCR0). */
size_t fpu_state_size(void);

/* XCR0: the state components every image holds. */
uint64_t fpu_xfeatures(void);

/* Bytes the XSAVE image takes in a signal frame: the image plus
 * FP_XSTATE_MAGIC2. */
size_t fpu_sigframe_size(void);

/* Allocate a thread's save area (64-byte aligned), filled with the clean
 * image. NULL on OOM. */
void *fpu_state_alloc(void);

/* Free a save area from fpu_state_alloc (NULL is fine). */
void fpu_state_free(void *state);

/* Save the live registers into dst, a save area (fork: the parent's state for
 * the child). */
void fpu_save_current(void *dst);

/* Load the clean image into the registers (execve, signal handler entry). */
void fpu_load_init(void);

/* Signal delivery: write the current thread's live state to user address
 * user_dst (64-byte aligned, fpu_sigframe_size() bytes) in Linux's format:
 * XSAVE image with fpstate->sw_reserved filled in, then FP_XSTATE_MAGIC2.
 * Uses the thread's own save area as scratch. Returns 0 or -EFAULT. */
int fpu_signal_save(struct Thread *t, uint64_t user_dst);

/* rt_sigreturn: read the image at user_src back into the registers. An image
 * without the magic numbers is taken as a plain FXSAVE image (x87 and SSE
 * only). Rejects anything XRSTOR would fault on: features outside XCR0, a
 * non-zero header or bad MXCSR bits. Returns 0, or -EFAULT for an unreadable
 * or corrupt image (the registers then hold an undefined mix: the caller kills
 * the process). user_src == 0 loads the clean image. */
int fpu_signal_restore(struct Thread *t, uint64_t user_src);

/* Context-switch hook, called by the scheduler just before context_switch:
 * saves old's FPU state, loads next's, and loads next's FS base if it differs. */
void arch_thread_switch(struct Thread *old, struct Thread *next);

/* Set the current thread's FS base (arch_prctl ARCH_SET_FS, execve). */
void arch_set_fs_base(uint64_t base);

#endif /* ARCHOS_ARCH_X86_64_FPU_H */
