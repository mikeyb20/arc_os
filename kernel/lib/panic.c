/* arc_os — Kernel panic report (M1-04)
 *
 * Owns: the panic sequence: claim the panic, stop the other CPUs, put the log
 * in panic mode, write reason / CPU / registers / backtrace with symbols, show
 * the panic screen, then halt (or end the QEMU run in test mode).
 * Invariants: one panic is reported per boot; panic_cpu is set once, by the
 * first CPU to panic. Nothing here allocates or sleeps.
 * Locks: none; the log's locks are bypassed once klog_panic_mode() runs. */

#include "lib/panic.h"
#include "lib/klog.h"
#include "lib/ksyms.h"
#include "lib/kprintf.h"
#include "lib/testmode.h"
#include "arch/hal.h"

#define PANIC_NOBODY 0xFFFFFFFFu

static uint32_t panic_cpu = PANIC_NOBODY;
static void (*panic_screen)(void);

void panic_set_screen(void (*show)(void)) {
    panic_screen = show;
}

void panic_halt(void) {
    for (;;) hal_halt_forever();
}

int panic_begin(void) {
    hal_disable_interrupts();
    uint32_t me = hal_cpu_index();
    uint32_t expected = PANIC_NOBODY;
    if (!__atomic_compare_exchange_n(&panic_cpu, &expected, me, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        if (expected == me) {
            /* Faulted inside our own panic report: say so on serial and stop */
            hal_serial_puts("\n[PANIC] fault while reporting a panic; stopped\n");
        }
        return 0;
    }
    klog_panic_mode();
    hal_stop_other_cpus();
    return 1;
}

void panic_report(const PanicInfo *info) {
    kprintf_level(KLOG_EMERG, "\n==================== KERNEL PANIC ====================\n");
    kprintf_level(KLOG_EMERG, "PANIC: %s on CPU %u\n", info->reason, info->cpu);
    for (uint32_t i = 0; i < info->nregs; i++) {
        kprintf_level(KLOG_EMERG, "%-6s %016lx%s", info->reg_names[i], info->reg_values[i],
                      (i % 4 == 3 || i + 1 == info->nregs) ? "\n" : "   ");
    }
    kprintf_level(KLOG_EMERG, "Backtrace:\n");
    for (uint32_t i = 0; i < info->nframes; i++) {
        uint64_t off = 0;
        const char *name = ksym_lookup(info->frames[i], &off);
        if (name != NULL) {
            kprintf_level(KLOG_EMERG, "  #%-2u %016lx %s+0x%lx\n", i, info->frames[i], name, off);
        } else {
            kprintf_level(KLOG_EMERG, "  #%-2u %016lx ?\n", i, info->frames[i]);
        }
    }
    kprintf_level(KLOG_EMERG, "======================================================\n");

    if (panic_screen != NULL) panic_screen();

    if (testmode_active()) {
        kprintf("ARC_TEST PANIC suite=%s\n", testmode_suite());
        testmode_exit(TESTMODE_PANIC_CODE);
    }
    panic_halt();
}

void kernel_panic_halt(void) {
    if (!panic_begin()) panic_halt();
    PanicInfo info = {.reason = "KERNEL_PANIC() (see the log lines above)"};
    hal_panic_capture(&info, __builtin_frame_address(0));
    panic_report(&info);
}
