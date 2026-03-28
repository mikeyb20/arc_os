#ifndef ARCHOS_LIBC_SYS_WAIT_H
#define ARCHOS_LIBC_SYS_WAIT_H

#include <sys/types.h>

/* Wait status macros */
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)     (((s) >> 8) & 0xFF)

#define WNOHANG 1

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* ARCHOS_LIBC_SYS_WAIT_H */
