#ifndef ARCHOS_SECURITY_HARDENING_H
#define ARCHOS_SECURITY_HARDENING_H

#include <stdint.h>

/* Stack canary support — __stack_chk_guard is the canary value,
 * __stack_chk_fail is called when corruption is detected. */
extern uint64_t __stack_chk_guard;

/* Initialize hardening: set up the stack canary value. */
void hardening_init(void);

#endif /* ARCHOS_SECURITY_HARDENING_H */
