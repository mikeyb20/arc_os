#ifndef ARCHOS_LIB_TESTMODE_H
#define ARCHOS_LIB_TESTMODE_H

#include <stdbool.h>

/* Kernel test mode (M0-02). Enabled only by `arc.test=<suite>` on the kernel
 * command line. In test mode a panic, or PID 1 exiting, ends the QEMU run at
 * once with an exit code (see testmode_exit). Outside test mode nothing changes. */

/* Maximum suite name length, including the NUL. */
#define TESTMODE_SUITE_MAX 64

/* Debug-exit value for a kernel panic. QEMU exits with (value << 1) | 1 = 255. */
#define TESTMODE_PANIC_CODE 0x7f

/* Debug-exit value when the CPU is below the x86-64-v3 floor (M1-01): QEMU
 * exits with 253. */
#define TESTMODE_CPU_FLOOR_CODE 0x7e

/* Read `arc.test=<suite>` from the command line. Call once, early in boot. */
void testmode_init(const char *cmdline);

/* True when the kernel was booted with arc.test=<suite>. */
bool testmode_active(void);

/* The suite name, or "" when not in test mode. */
const char *testmode_suite(void);

/* Record the PID of init, so testmode_process_exited can recognise it. */
void testmode_set_init_pid(int pid);

/* Register a function run once when init exits, just before the run ends
 * (end-of-run budget metrics). Not run on a panic. NULL clears it. */
void testmode_set_end_hook(void (*fn)(void));

/* Called when any process exits (normally or by signal) with its status.
 * In test mode, init exiting ends the run: QEMU exits with init's status
 * (clamped to 0..0x3e; 0 = pass). Otherwise does nothing. */
void testmode_process_exited(int pid, int status);

/* End the run: write `code` to the QEMU isa-debug-exit port, so QEMU exits with
 * (code << 1) | 1. Only codes 0..0x7f are used (PID 1 status is masked to
 * 0..0x3f). If no debug-exit device is present, halts. Never returns. */
void testmode_exit(int code) __attribute__((noreturn));

/* Fatal stop used by KERNEL_PANIC() and unhandled CPU exceptions. In test mode
 * prints "ARC_TEST PANIC" and exits QEMU with TESTMODE_PANIC_CODE; otherwise
 * halts this CPU forever. Never returns. */
void kernel_panic_halt(void) __attribute__((noreturn));

#endif /* ARCHOS_LIB_TESTMODE_H */
