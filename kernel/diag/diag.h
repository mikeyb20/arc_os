#ifndef ARCHOS_DIAG_DIAG_H
#define ARCHOS_DIAG_DIAG_H

#include "boot/bootinfo.h"

/* Boot diagnostics for real-hardware bring-up (M1-04): the boot report, the
 * self-test (arc.selftest=1) and the benchmarks (arc.bench=1).
 *
 * Every boot logs the report as "[REPORT] ..." lines. With arc.report=1,
 * arc.selftest=1 or arc.bench=1 the kernel instead stops after the report,
 * runs the tests asked for, and pages the whole report on screen every 10 s
 * (any key: next page) instead of starting init. In test mode it ends the
 * QEMU run after one pass. */

/* Write the boot report to the log (and keep it for the pager when a
 * diagnostic mode is on). Call after SMP, ACPI and PCI are up. */
void diag_report(const BootInfo *info);

/* True when arc.report, arc.selftest or arc.bench asks for diagnostic mode. */
int diag_requested(void);

/* Diagnostic mode: self-test and benchmarks if asked, then the pager,
 * forever. Needs interrupts on and the PIT running. */
void diag_run(void) __attribute__((noreturn));

/* ---- Shared by the diag files ---- */

/* Add one report line (also logged as "[REPORT] <line>"). */
void diag_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Run the self-test; stores the number of passed and failed checks. */
void diag_selftest(int *passes, int *fails);

/* Run the benchmarks and add their table to the report. */
void diag_bench(void);

/* Busy-wait ms milliseconds by the TSC (diagnostics only). */
void diag_delay_ms(uint32_t ms);

#endif /* ARCHOS_DIAG_DIAG_H */
