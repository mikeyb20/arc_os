#ifndef ARCHOS_DIAG_DIAG_H
#define ARCHOS_DIAG_DIAG_H

#include "boot/bootinfo.h"
#include "lib/panic.h"

/* Boot diagnostics for real-hardware bring-up (M1-04): the boot report, the
 * self-test (arc.selftest=1) and the benchmarks (arc.bench=1).
 *
 * Every boot logs the report as "[REPORT] ..." lines. With arc.report=1,
 * arc.selftest=1, arc.bench=1 or arc.qr the kernel instead stops after the
 * report, runs the tests asked for, and pages the whole report on screen every
 * 10 s (any key: next page) instead of starting init. arc.qr=1 shows the pages
 * as QR codes, arc.qr=log the newest 8 KiB of the kernel log (M1-04c). In
 * test mode it ends the QEMU run after one pass. */

/* Write the boot report to the log (and keep it for the pager when a
 * diagnostic mode is on). Call after SMP, ACPI and PCI are up. */
void diag_report(const BootInfo *info);

/* True when arc.report, arc.selftest, arc.bench or arc.qr asks for
 * diagnostic mode. */
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

/* Command-line flag "key=1": any value but 0 counts, a bare key too. */
int diag_flag_on(const char *key);

/* Report lines kept for the pager (0 / "" outside diagnostic mode). */
uint32_t diag_line_count(void);
const char *diag_line_text(uint32_t i);

/* Wait up to `seconds` (TSC time) or until a key is pressed; returns 1 for a
 * key. Halts between interrupts when can_halt (the timer is known to tick),
 * otherwise polls. */
int diag_wait_key(uint32_t seconds, int can_halt);

/* ---- QR screens (diag/qr_mode.c, M1-04c) ---- */

/* arc.qr: show the report (arc.qr=1) or the log tail (arc.qr=log) as QR
 * pages, forever; in test mode one pass, waiting for a key after each page,
 * then end the run (passes and fails from the self-test count too). Returns
 * only when the screen can't show a code: the caller falls back to text. */
void diag_qr_run(int can_halt, int passes, int fails);

/* Read the QR panic options once at boot (arc.qrhold, test mode only). */
void diag_qr_panic_setup(void);

/* Panic screen hook (panic_set_screen): the panic view, plus one QR code of
 * kind "panic" with the reason, CPU, registers and backtrace. Allocates
 * nothing and takes no lock. */
void diag_qr_panic_screen(const PanicInfo *info);

#endif /* ARCHOS_DIAG_DIAG_H */
