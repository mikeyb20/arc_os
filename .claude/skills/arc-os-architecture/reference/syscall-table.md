# Syscall Table Reference

## Implementation State

> **Note**: SYSCALL/SYSRET infrastructure exists in source files (`kernel/arch/x86_64/syscall.h`, `syscall.c`, `syscall_entry.asm`, `msr.h`) but is **not yet compiled into the kernel**. The scaffolded code defines `SYSCALL_MAX=64` and registers handlers via `syscall_register()`. It will be compiled and integrated in Phase 5.

## Overview

System calls are the user-kernel boundary. The syscall table is the contract — syscall numbers are stable once assigned and never reused.

## Calling Convention (x86_64)

- Entry: `SYSCALL` instruction (fast path, no interrupt overhead)
- Return: `SYSRET` instruction
- Syscall number: `RAX`
- Arguments: `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9` (up to 6 arguments)
- Return value: `RAX` (negative values indicate errors, `-errno`)
- Clobbered by kernel: `RCX`, `R11` (used by SYSCALL/SYSRET hardware)

## Syscall Table

Populated as implemented. Reserved number ranges:

| Range | Category |
|-------|----------|
| 0-63 | Core process/memory |
| 64-127 | File/filesystem |
| 128-191 | IPC/signals |
| 192-255 | Network |
| 256-319 | System info/control |
| 320+ | Extensions |

### Scaffolded Numbering (in `kernel/arch/x86_64/syscall.h`)

These are the numbers defined in the scaffolded source code (not yet compiled):

| Number | Name | Notes |
|--------|------|-------|
| 0 | `SYS_EXIT` | |
| 1 | `SYS_WRITE` | |
| 2 | `SYS_GETPID` | |
| 3 | `SYS_OPEN` | |
| 4 | `SYS_READ` | |
| 5 | `SYS_CLOSE` | |
| 6 | `SYS_BRK` | |

Note: the scaffolded numbering differs from the reference plan below (e.g., `GETPID` is 2 in scaffolded code vs 8 in the plan). Numbering will be finalized when Phase 5 is implemented.

### Reference Plan (Phase 5)

| Number | Name | Signature | Description |
|--------|------|-----------|-------------|
| 0 | `sys_exit` | `void exit(int status)` | Terminate process |
| 1 | `sys_write` | `ssize_t write(int fd, const void *buf, size_t count)` | Write to file descriptor |
| 2 | `sys_read` | `ssize_t read(int fd, void *buf, size_t count)` | Read from file descriptor |
| 3 | `sys_open` | `int open(const char *path, int flags, mode_t mode)` | Open a file |
| 4 | `sys_close` | `int close(int fd)` | Close file descriptor |
| 5 | `sys_fork` | `pid_t fork(void)` | Create child process |
| 6 | `sys_exec` | `int execve(const char *path, char **argv, char **envp)` | Execute program |
| 7 | `sys_wait` | `pid_t waitpid(pid_t pid, int *status, int options)` | Wait for child |
| 8 | `sys_getpid` | `pid_t getpid(void)` | Get process ID |
| 9 | `sys_brk` | `void *brk(void *addr)` | Set program break |
| 10 | `sys_mmap` | `void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)` | Map memory |

### Additional Syscalls (added as needed)

| Number | Name | Description |
|--------|------|-------------|
| 11 | `sys_munmap` | Unmap memory |
| 12 | `sys_dup2` | Duplicate file descriptor |
| 13 | `sys_pipe` | Create pipe |
| 14 | `sys_ioctl` | Device control |
| 15 | `sys_lseek` | Seek in file |
| 16 | `sys_stat` | File status |
| 17 | `sys_mkdir` | Create directory |
| 18 | `sys_rmdir` | Remove directory |
| 19 | `sys_unlink` | Remove file |
| 20 | `sys_chdir` | Change directory |
| 64 | `sys_kill` | Send signal |
| 65 | `sys_signal` | Set signal handler |

## Argument Validation

Every pointer argument from user space MUST be validated before dereference:
1. Check pointer is in user-space range (below `0x00007FFFFFFFFFFF`)
2. Check memory is mapped and accessible
3. Check buffer length doesn't overflow into kernel space
