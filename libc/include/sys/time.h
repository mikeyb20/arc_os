#ifndef ARCHOS_LIBC_SYS_TIME_H
#define ARCHOS_LIBC_SYS_TIME_H

/* struct timeval and struct timezone (Linux x86_64, ADR-0001) */
#include <uapi/time.h>

/* Wall-clock time in *tv; *tz (obsolete) gets zeros. Either may be NULL.
 * 0, or -1 with errno (EFAULT). */
int gettimeofday(struct timeval *tv, struct timezone *tz);

#endif /* ARCHOS_LIBC_SYS_TIME_H */
