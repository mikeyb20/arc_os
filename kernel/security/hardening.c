/* arc_os — kernel hardening: stack canaries (M0), NX, SMEP, SMAP and W^X
 * (M1-19, hardening.h).
 *
 * Owns: the stack canary, turning the CPU protections on for the boot CPU
 * (APs: smp.c) with a check of the kernel's mappings, and the test-mode
 * triggers that prove each protection by panicking.
 * Invariants: the boot CPU has NX on and the kernel image mapped W^X before
 * anything below the VMM runs, or the boot stops.
 * Locks: none (boot thread).
 * Cost: one page-table walk per kernel image page at boot (about 150). */

#include "security/hardening.h"
#include "arch/hal.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "lib/string.h"
#include <stddef.h>

/* Stack canary value — initialized to a semi-random value at boot.
 * GCC's -fstack-protector-strong generates code that checks this. */
uint64_t __stack_chk_guard;

/* Called by GCC-generated code when a stack buffer overflow is detected. */
void __stack_chk_fail(void) {
    kprintf("\n[SECURITY] *** STACK SMASHING DETECTED ***\n");
    kprintf("[SECURITY] Stack canary corruption — halting.\n");
    KERNEL_PANIC();
}

void hardening_init(void) {
    /* Initialize stack canary with a value that's unlikely to appear
     * in normal data.  We use the PIT tick count XOR'd with a constant
     * for minimal entropy (no RDRAND in all environments). */
    __stack_chk_guard = 0x00000AFF0DEAD000ULL;

    /* Try to use RDTSC for some entropy if available */
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    __stack_chk_guard ^= ((uint64_t)hi << 32) | lo;

    /* Ensure the canary has a null byte at position 0 (catches string overflows) */
    __stack_chk_guard &= ~0xFFULL;

    kprintf("[SECURITY] Stack canaries enabled (guard=0x%lx)\n", __stack_chk_guard);
}

/* ---- NX, SMEP, SMAP and kernel W^X (M1-19) ---- */

void hardening_protect_boot_cpu(void) {
    hal_protect_cpu_init();
    uint32_t f = hal_protect_flags();
    VmmWxCheck wx;
    int rc = vmm_kernel_wx_check(&wx);
    kprintf("[SECURITY] NX %s, SMEP %s, SMAP %s; kernel W^X: %lu code pages r-x, %lu read-only, %lu data rw-\n",
            (f & HAL_PROTECT_NX) ? "on" : "OFF", (f & HAL_PROTECT_SMEP) ? "on" : "off (CPU lacks it)",
            (f & HAL_PROTECT_SMAP) ? "on" : "off (CPU lacks it)", wx.text_pages, wx.ro_pages, wx.data_pages);
    if (rc != 0 || !(f & HAL_PROTECT_NX) || !(f & HAL_PROTECT_WP)) {
        kprintf("[SECURITY] FATAL: kernel mappings wrong: %lu writable+executable, %lu not as their part says "
                "(first at 0x%lx), HHDM %s, CR0.WP %s\n",
                wx.wx_pages, wx.wrong_pages, wx.first_wrong, wx.hhdm_ok ? "ok" : "not rw-/NX",
                (f & HAL_PROTECT_WP) ? "on" : "OFF");
        KERNEL_PANIC();
    }
}

/* ---- Test mode: suite kpanic, arc.panic=<kind> ---- */

#define TEST_USER_PAGE 0x400000UL   /* a user address no kernel page table uses */

/* Each test below must panic; returning means the protection is missing */

__attribute__((noinline)) static void hardening_test_text(void) {
    kprintf("[TEST] W^X: writing kernel code at 0x%lx\n", (uint64_t)(uintptr_t)hardening_test_text);
    *(volatile uint8_t *)(uintptr_t)hardening_test_text = 0xCC;
}

static const volatile uint64_t rodata_probe = 0x5A5A5A5A5A5A5A5AULL;

__attribute__((noinline)) static void hardening_test_rodata(void) {
    kprintf("[TEST] W^X: writing .rodata at 0x%lx\n", (uint64_t)(uintptr_t)&rodata_probe);
    *(volatile uint64_t *)(uintptr_t)&rodata_probe = 0;
}

static uint8_t data_probe[16] = {0xC3};   /* RET, in .data */

__attribute__((noinline)) static void hardening_test_nx(void) {
    kprintf("[TEST] W^X: calling .data at 0x%lx\n", (uint64_t)(uintptr_t)data_probe);
    ((void (*)(void))(uintptr_t)data_probe)();
}

/* Map one user page (U bit) at TEST_USER_PAGE in the kernel's own page
 * tables, holding RET; only the boot thread runs on them at this point */
static int map_test_user_page(uint32_t flags) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) return -1;
    uint8_t *p = (uint8_t *)(uintptr_t)(phys + vmm_get_hhdm_offset());
    memset(p, 0xC3, PAGE_SIZE);
    if (vmm_map_page_in(vmm_get_kernel_pml4(), TEST_USER_PAGE, phys, VMM_FLAG_USER | flags) != 0) return -1;
    hal_invalidate_page(TEST_USER_PAGE);
    return 0;
}

__attribute__((noinline)) static void hardening_test_smap(void) {
    if (map_test_user_page(VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC) != 0) return;
    kprintf("[TEST] SMAP: reading user page 0x%lx directly\n", TEST_USER_PAGE);
    (void)*(volatile uint64_t *)(uintptr_t)TEST_USER_PAGE;
}

__attribute__((noinline)) static void hardening_test_smep(void) {
    if (map_test_user_page(0) != 0) return;
    kprintf("[TEST] SMEP: calling user page 0x%lx\n", TEST_USER_PAGE);
    ((void (*)(void))(uintptr_t)TEST_USER_PAGE)();
}

void hardening_test(const char *kind) {
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"text", hardening_test_text}, {"rodata", hardening_test_rodata}, {"nx", hardening_test_nx},
        {"smap", hardening_test_smap}, {"smep", hardening_test_smep},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (strcmp(kind, tests[i].name) != 0) continue;
        tests[i].fn();
        kprintf("[TEST] arc.panic=%s: the access went through (protection missing)\n", kind);
        KERNEL_PANIC();
    }
}
