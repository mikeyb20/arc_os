#ifndef ARCHOS_SECURITY_HARDENING_H
#define ARCHOS_SECURITY_HARDENING_H

#include <stdint.h>

/* Stack canary support — __stack_chk_guard is the canary value,
 * __stack_chk_fail is called when corruption is detected. */
extern uint64_t __stack_chk_guard;

/* Initialize hardening: set up the stack canary value. */
void hardening_init(void);

/* Turn on WP, NX, SMEP and SMAP on the boot CPU, then check the kernel's page
 * tables: code read + execute, read-only data and Limine's requests
 * read-only + NX, data read-write + NX, the HHDM NX (M1-19). Logs one line;
 * stops the boot if a mapping is wrong or NX or WP is off. Call right after
 * vmm_init, on the BSP. */
void hardening_protect_boot_cpu(void);

/* Test mode only (suite kpanic, arc.panic=<kind>): break one protection on
 * purpose; each must end in a panic. kind: "text" (write kernel code),
 * "rodata" (write read-only data), "nx" (run .data), "smap" (read a user
 * page directly), "smep" (run a user page). Unknown kinds return. */
void hardening_test(const char *kind);

#endif /* ARCHOS_SECURITY_HARDENING_H */
