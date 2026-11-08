#ifndef ARCHOS_LIBC_SYS_AUXV_H
#define ARCHOS_LIBC_SYS_AUXV_H

#include <uapi/auxvec.h>

/* Value of auxiliary vector entry type, or 0 with errno = ENOENT. */
unsigned long getauxval(unsigned long type);

#endif /* ARCHOS_LIBC_SYS_AUXV_H */
