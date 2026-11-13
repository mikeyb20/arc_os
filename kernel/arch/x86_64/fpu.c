/* arc_os — user FPU/SSE/AVX state with XSAVE, and FS base switching (see fpu.h). */

#include "arch/x86_64/fpu.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/cpuid.h"
#include "arch/x86_64/msr.h"
#include "proc/thread.h"
#include "arch/hal_lock.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "uapi/sigcontext.h"
#include "uapi/errno.h"
#include "user_access.h"

#define CR0_MP         (1ULL << 1)
#define CR0_EM         (1ULL << 2)
#define CR0_TS         (1ULL << 3)
#define CR0_NE         (1ULL << 5)
#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_OSXSAVE    (1ULL << 18)

/* XCR0 state components */
#define XFEATURE_X87    (1ULL << 0)
#define XFEATURE_SSE    (1ULL << 1)
#define XFEATURE_AVX    (1ULL << 2)
#define XFEATURE_AVX512 (0x7ULL << 5)   /* opmask, ZMM_Hi256, Hi16_ZMM */

#define CPUID7_EBX_AVX512F   (1u << 16)
#define CPUIDD1_EAX_XSAVEOPT (1u << 0)

#define FCW_DEFAULT   0x037Fu   /* all x87 exceptions masked, 64-bit precision */
#define MXCSR_DEFAULT 0x1F80u   /* all SSE exceptions masked, round to nearest */

/* Largest image this kernel accepts: x87 + SSE + AVX + AVX-512 is 2688 bytes
 * in the standard format. AMX is never enabled. */
#define FPU_IMAGE_MAX 4096

static uint64_t xcr0;          /* components every image holds */
static uint32_t xsave_size;    /* bytes per image */
static int have_xsaveopt;
static uint32_t mxcsr_mask;    /* MXCSR bits XRSTOR accepts */

/* Clean image every new thread, every execve and every signal handler starts
 * from; also used for the MXCSR mask probe */
static uint8_t fpu_init_image[FPU_IMAGE_MAX] __attribute__((aligned(FPU_STATE_ALIGN)));

static inline void xsetbv(uint32_t reg, uint64_t value) {
    __asm__ volatile("xsetbv" : : "c"(reg), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* All three take the requested-feature bitmap in EDX:EAX; all ones = XCR0 */
static inline void xsave(void *area) {
    __asm__ volatile("xsave64 (%0)" : : "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu) : "memory");
}

static inline void xsaveopt(void *area) {
    __asm__ volatile("xsaveopt64 (%0)" : : "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu) : "memory");
}

static inline void xrstor(const void *area) {
    __asm__ volatile("xrstor64 (%0)" : : "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu) : "memory");
}

/* The XSAVE header's first word: which components the image holds */
static inline uint64_t *xstate_bv(void *area) {
    return (uint64_t *)((uint8_t *)area + XSAVE_LEGACY_SIZE);
}

/* XCR0 = x87 | SSE | AVX, plus AVX-512 when the CPU has it and XSAVE can
 * hold it (ADR-0003); the BSP picks, the APs copy. */
static uint64_t fpu_pick_xcr0(void) {
    uint32_t a, b, c, d;
    cpuid(0xD, 0, &a, &b, &c, &d);
    uint64_t supported = ((uint64_t)d << 32) | a;
    uint64_t want = XFEATURE_X87 | XFEATURE_SSE | XFEATURE_AVX;
    if ((cpu_features()->l7_ebx & CPUID7_EBX_AVX512F) && (supported & XFEATURE_AVX512) == XFEATURE_AVX512)
        want |= XFEATURE_AVX512;
    return want & supported;
}

/* BSP only: size the area, find the MXCSR mask, build the clean image */
static void fpu_bsp_setup(void) {
    uint32_t a, b, c, d;
    cpuid(0xD, 0, &a, &b, &c, &d);   /* EBX: size for the XCR0 just set */
    xsave_size = b;
    cpuid(0xD, 1, &a, &b, &c, &d);
    have_xsaveopt = (a & CPUIDD1_EAX_XSAVEOPT) != 0;
    if (xsave_size < XSAVE_LEGACY_SIZE + XSAVE_HEADER_SIZE || xsave_size > FPU_IMAGE_MAX) {
        kprintf("[FPU] FATAL: XSAVE area of %u bytes not supported\n", xsave_size);
        KERNEL_PANIC();
    }

    /* MXCSR_MASK from an FXSAVE image (0 there means the default 0xFFBF) */
    memset(fpu_init_image, 0, sizeof(fpu_init_image));
    __asm__ volatile("fxsave64 (%0)" : : "r"(fpu_init_image) : "memory");
    struct _fpstate *fx = (struct _fpstate *)fpu_init_image;
    mxcsr_mask = fx->mxcsr_mask ? fx->mxcsr_mask : 0xFFBFu;

    /* Clean image: x87 and SSE from memory (FCW, MXCSR, zero registers), every
     * other component in its init state (all zero) */
    memset(fpu_init_image, 0, sizeof(fpu_init_image));
    fx->cwd = FCW_DEFAULT;
    fx->mxcsr = MXCSR_DEFAULT;
    fx->mxcsr_mask = mxcsr_mask;
    *xstate_bv(fpu_init_image) = XFEATURE_X87 | XFEATURE_SSE;
}

void fpu_cpu_init(int is_bsp) {
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 | CR0_MP | CR0_NE) & ~(CR0_EM | CR0_TS);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_OSXSAVE;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    if (is_bsp) xcr0 = fpu_pick_xcr0();
    xsetbv(0, xcr0);
    if (is_bsp) fpu_bsp_setup();

    xrstor(fpu_init_image);
    wrmsr(MSR_FS_BASE, 0);   /* every thread starts with fs_base 0 */
    if (is_bsp) {
        kprintf("[FPU] XSAVE on: XCR0=0x%lx (%s), %u-byte area per thread, %s\n", xcr0,
                (xcr0 & XFEATURE_AVX512) ? "AVX-512" : "AVX", xsave_size, have_xsaveopt ? "XSAVEOPT" : "XSAVE");
    }
}

size_t fpu_state_size(void) {
    return xsave_size;
}

uint64_t fpu_xfeatures(void) {
    return xcr0;
}

size_t fpu_sigframe_size(void) {
    return (size_t)xsave_size + FP_XSTATE_MAGIC2_SIZE;
}

/* kmalloc aligns to 16: over-allocate, align to 64 and keep the pointer
 * kmalloc returned just below the area for fpu_state_free */
void *fpu_state_alloc(void) {
    uint8_t *raw = kmalloc(xsave_size + FPU_STATE_ALIGN + sizeof(void *), 0);
    if (raw == NULL) return NULL;
    uintptr_t area = ((uintptr_t)raw + sizeof(void *) + FPU_STATE_ALIGN - 1) & ~(uintptr_t)(FPU_STATE_ALIGN - 1);
    ((void **)area)[-1] = raw;
    memcpy((void *)area, fpu_init_image, xsave_size);
    return (void *)area;
}

void fpu_state_free(void *state) {
    if (state != NULL) kfree(((void **)state)[-1]);
}

void fpu_save_current(void *dst) {
    xsave(dst);   /* plain XSAVE: dst is not where these registers came from */
}

void fpu_load_init(void) {
    xrstor(fpu_init_image);
}

int fpu_signal_save(Thread *t, uint64_t user_dst) {
    xsave(t->fpu_state);
    struct _fpstate *fx = (struct _fpstate *)t->fpu_state;
    memset(&fx->sw_reserved, 0, sizeof(fx->sw_reserved));   /* bytes XSAVE leaves alone */
    fx->sw_reserved.magic1 = FP_XSTATE_MAGIC1;
    fx->sw_reserved.extended_size = (uint32_t)fpu_sigframe_size();
    fx->sw_reserved.xfeatures = xcr0;
    fx->sw_reserved.xstate_size = xsave_size;
    uint32_t magic2 = FP_XSTATE_MAGIC2;
    if (copy_to_user((void *)user_dst, t->fpu_state, xsave_size) != 0 ||
        copy_to_user((void *)(user_dst + xsave_size), &magic2, sizeof(magic2)) != 0)
        return -EFAULT;
    return 0;
}

int fpu_signal_restore(Thread *t, uint64_t user_src) {
    if (user_src == 0) {
        fpu_load_init();
        return 0;
    }
    /* Build the image in the thread's own area (scratch while it runs: its
     * live state is in the registers), check it, then XRSTOR from there. This
     * relies on nothing switching the thread out in between, which holds while
     * syscalls run with IF clear and copy_from_user can't sleep (revisit in
     * M1-05/M1-07). On failure the area is reset to the init image, because a
     * later switch-in XRSTORs from it and a bad header would #GP in the
     * kernel. */
    uint8_t *area = (uint8_t *)t->fpu_state;
    struct _fpstate *fx = (struct _fpstate *)area;
    if (copy_from_user(area, (const void *)user_src, XSAVE_LEGACY_SIZE) != 0) goto bad;

    struct _fpx_sw_bytes sw = fx->sw_reserved;
    uint32_t magic2 = 0;
    int full = sw.magic1 == FP_XSTATE_MAGIC1 && sw.xstate_size >= XSAVE_LEGACY_SIZE + XSAVE_HEADER_SIZE &&
               sw.xstate_size <= xsave_size && sw.extended_size == sw.xstate_size + FP_XSTATE_MAGIC2_SIZE &&
               copy_from_user(&magic2, (const void *)(user_src + sw.xstate_size), sizeof(magic2)) == 0 &&
               magic2 == FP_XSTATE_MAGIC2;
    if (full) {
        if (copy_from_user(area + XSAVE_LEGACY_SIZE, (const void *)(user_src + XSAVE_LEGACY_SIZE),
                           sw.xstate_size - XSAVE_LEGACY_SIZE) != 0)
            goto bad;
        memset(area + sw.xstate_size, 0, xsave_size - sw.xstate_size);
        /* XRSTOR faults on header bytes 8..63 that aren't zero (XCOMP_BV and
         * reserved) and on components outside XCR0 */
        const uint8_t *hdr = area + XSAVE_LEGACY_SIZE;
        for (int i = 8; i < XSAVE_HEADER_SIZE; i++) {
            if (hdr[i] != 0) goto bad;
        }
        if (*xstate_bv(area) & ~xcr0) goto bad;
        *xstate_bv(area) &= sw.xfeatures;   /* components the frame says it has */
    } else {
        /* Plain FXSAVE image: x87 and SSE from it, the rest in init state */
        memset(area + XSAVE_LEGACY_SIZE, 0, xsave_size - XSAVE_LEGACY_SIZE);
        *xstate_bv(area) = XFEATURE_X87 | XFEATURE_SSE;
    }
    if (fx->mxcsr & ~mxcsr_mask) goto bad;   /* XRSTOR would #GP */
    xrstor(area);
    return 0;
bad:
    memcpy(area, fpu_init_image, xsave_size);
    return -EFAULT;
}

void arch_thread_switch(Thread *old, Thread *next) {
    /* Threads without an area (the boot thread) never run user code. Every
     * switch-in restores from the thread's own area, so XSAVEOPT's "unchanged
     * since the last XRSTOR from here" tracking stays correct. */
    if (old->fpu_state != NULL) {
        if (have_xsaveopt)
            xsaveopt(old->fpu_state);
        else
            xsave(old->fpu_state);
    }
    if (next->fpu_state != NULL) xrstor(next->fpu_state);
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

/* The AVX2 routine in selftest_avx2.asm (M1-04) */
extern uint32_t selftest_avx2_sum(const uint32_t *buf, uint64_t count);

int fpu_avx2_selftest(const uint32_t *buf, uint64_t count, uint32_t *sum, uint64_t *xcr0_out) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    uint64_t here = 0;
    if (cr4 & CR4_OSXSAVE) {
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        here = ((uint64_t)hi << 32) | lo;
    }
    *xcr0_out = here;
    if (here != xcr0 || (here & (XFEATURE_SSE | XFEATURE_AVX)) != (XFEATURE_SSE | XFEATURE_AVX)) return -1;
    *sum = selftest_avx2_sum(buf, count);
    return 0;
}
