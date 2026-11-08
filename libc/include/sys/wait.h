#ifndef ARCHOS_LIBC_WAIT_H
#define ARCHOS_LIBC_WAIT_H

#include <sys/types.h>

/* WNOHANG, WUNTRACED, WCONTINUED (Linux values, ADR-0001) */
#include <uapi/wait.h>

/* Status word decoding, Linux encoding (see uapi/wait.h) */
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WIFSIGNALED(s)  (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7F)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* ARCHOS_LIBC_WAIT_H */
