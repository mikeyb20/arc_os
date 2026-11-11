#ifndef ARCHOS_LIB_PANIC_H
#define ARCHOS_LIB_PANIC_H

#include <stdint.h>

/* Kernel panic report (M1-04): what went wrong, on which CPU, the registers
 * and a symbolised backtrace, written to the log (serial) and shown on screen
 * as a colour block meant to be photographed. The architecture fills in the
 * registers and the backtrace (hal.h: hal_panic_*); this part is portable. */

#define PANIC_MAX_REGS   24
#define PANIC_MAX_FRAMES 16

typedef struct {
    const char *reason;                     /* e.g. "Page Fault", "KERNEL_PANIC()" */
    uint32_t cpu;                           /* CPU index */
    uint32_t nregs;
    const char *reg_names[PANIC_MAX_REGS];  /* "RIP", "RSP", ..., "CR2" */
    uint64_t reg_values[PANIC_MAX_REGS];
    uint32_t nframes;
    uint64_t frames[PANIC_MAX_FRAMES];      /* return addresses, innermost first */
} PanicInfo;

/* Start a panic on this CPU. Returns 1 for the first panic: the log stops
 * waiting for locks and the other CPUs are stopped. A panic that starts while
 * one is already being reported (a fault in the panic path, or a second CPU)
 * gets 0 and should stop at once (panic_halt). */
int panic_begin(void);

/* Write the report to the log, show the panic screen, then stop: in test mode
 * print "ARC_TEST PANIC" and exit QEMU, otherwise halt forever. */
void panic_report(const PanicInfo *info) __attribute__((noreturn));

/* Stop this CPU for good (interrupts off). */
void panic_halt(void) __attribute__((noreturn));

/* Register the screen view that panic_report calls after logging the report
 * (fb_console_panic_view): it shows the end of the log in panic colours. */
void panic_set_screen(void (*show)(void));

#endif /* ARCHOS_LIB_PANIC_H */
