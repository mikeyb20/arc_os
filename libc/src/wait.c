/* arc_os libc — wait/waitpid over wait4 */

#include <sys/wait.h>
#include <errno.h>
#include "syscall_arch.h"

pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)syscall_ret(syscall4(__NR_wait4, (uint64_t)(int64_t)pid, (uint64_t)status,
                                       (uint64_t)options, 0));
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}
