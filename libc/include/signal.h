#ifndef ARCHOS_LIBC_SIGNAL_H
#define ARCHOS_LIBC_SIGNAL_H

#include <sys/types.h>

/* Signal numbers — must match kernel/proc/signal.h */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGKILL  9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

typedef void (*sig_t)(int);

sig_t signal(int signo, sig_t handler);
int   kill(pid_t pid, int signo);

#endif /* ARCHOS_LIBC_SIGNAL_H */
