/* arc_os — user FPU/SSE state and FS base switching (see fpu.h). */

#include "arch/x86_64/fpu.h"
#include "arch/x86_64/msr.h"
#include "proc/thread.h"
#include "proc/spinlock.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

#define CR0_MP        (1ULL << 1)
#define CR0_EM        (1ULL << 2)
#define CR0_TS        (1ULL << 3)
#define CR0_NE        (1ULL << 5)
#define CR4_OSFXSR    (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)

#define MXCSR_DEFAULT 0x1F80u   /* all exceptions masked, round to nearest */

/* Clean image every new thread and every execve starts from */
static uint8_t fpu_init_image[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));

_Static_assert(FPU_STATE_ALIGN <= 16, "kmalloc aligns to 16 bytes");

static inline void fxsave(void *area) {
    __asm__ volatile ("fxsave64 (%0)" : : "r"(area) : "memory");
}

static inline void fxrstor(const void *area) {
    __asm__ volatile ("fxrstor64 (%0)" : : "r"(area) : "memory");
}

void fpu_cpu_init(int is_bsp) {
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 | CR0_MP | CR0_NE) & ~(CR0_EM | CR0_TS);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    uint32_t mxcsr = MXCSR_DEFAULT;
    __asm__ volatile ("fninit");                       /* FCW = 0x37F */
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
    wrmsr(MSR_FS_BASE, 0);   /* every thread starts with fs_base 0 */
    if (is_bsp) {
        /* fninit leaves the XMM registers alone: zero them so the image is clean */
        memset(fpu_init_image, 0, sizeof(fpu_init_image));
        fxsave(fpu_init_image);
        memset(fpu_init_image + 160, 0, 16 * 16);      /* XMM0-15 */
        fxrstor(fpu_init_image);
        kprintf("[FPU] SSE enabled, %u-byte FXSAVE area per thread\n", FPU_STATE_SIZE);
    }
}

void *fpu_state_alloc(void) {
    void *s = kmalloc(FPU_STATE_SIZE, 0);
    if (s != NULL) memcpy(s, fpu_init_image, FPU_STATE_SIZE);
    return s;
}

void fpu_state_free(void *state) {
    if (state != NULL) kfree(state);
}

void fpu_save_current(void *dst) {
    fxsave(dst);
}

void fpu_load_init(void) {
    fxrstor(fpu_init_image);
}

void arch_thread_switch(Thread *old, Thread *next) {
    /* Threads without an area (the boot thread) never run user code */
    if (old->fpu_state != NULL) fxsave(old->fpu_state);
    if (next->fpu_state != NULL) fxrstor(next->fpu_state);
    /* Invariant: MSR_FS_BASE == running thread's fs_base */
    if (next->fs_base != old->fs_base) wrmsr(MSR_FS_BASE, next->fs_base);
}

void arch_set_fs_base(uint64_t base) {
    /* Field and MSR change together: a switch in between would break the
     * invariant arch_thread_switch relies on */
    uint64_t flags = irq_save();
    thread_current()->fs_base = base;
    wrmsr(MSR_FS_BASE, base);
    irq_restore(flags);
}
