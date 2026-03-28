#ifndef ARCHOS_SECURITY_HARDENING_H
#define ARCHOS_SECURITY_HARDENING_H

#include <stdint.h>

/* Stack canary support — __stack_chk_guard is the canary value,
 * __stack_chk_fail is called when corruption is detected. */
extern uint64_t __stack_chk_guard;

/* Initialize hardening: set up stack canary value, apply guard pages. */
void hardening_init(void);

/* Apply a guard page (unmapped page) below the given stack base.
 * stack_base_virt: virtual address of the stack's lowest byte.
 * The guard page is placed at (stack_base_virt - PAGE_SIZE). */
void hardening_add_guard_page(uint64_t stack_base_virt);

#endif /* ARCHOS_SECURITY_HARDENING_H */
