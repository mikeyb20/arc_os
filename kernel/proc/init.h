#ifndef ARCHOS_PROC_INIT_H
#define ARCHOS_PROC_INIT_H

#include "boot/bootinfo.h"

/* Launch the init (PID 1) user-space process from boot modules.
 * Returns 0 on success, negative on failure. */
int init_launch(const BootInfo *info);

#endif /* ARCHOS_PROC_INIT_H */
