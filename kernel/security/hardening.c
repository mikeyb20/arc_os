#include "security/hardening.h"
#include "lib/kprintf.h"

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
