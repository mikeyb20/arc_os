#ifndef ARCHOS_LIBC_SIGNAL_H
#define ARCHOS_LIBC_SIGNAL_H

#include <sys/types.h>

/* Signal numbers, SA_* flags and the kernel sigaction layout (Linux, ADR-0001) */
#include <uapi/signal.h>

typedef void (*sig_t)(int);

#define SIG_DFL ((sig_t)0)
#define SIG_IGN ((sig_t)1)
#define SIG_ERR ((sig_t)-1)

/* Install handler for signo (via rt_sigaction). Returns the previous handler,
 * or SIG_ERR with errno set. */
sig_t signal(int signo, sig_t handler);

/* Send signo to pid (> 0), the caller's group (0) or group -pid (< 0). */
int   kill(pid_t pid, int signo);

#endif /* ARCHOS_LIBC_SIGNAL_H */
