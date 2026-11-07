# Syscall Table Reference

> Binding decision: [ADR-0001](../../../../docs/decisions/0001-linux-syscall-abi.md) — arc_os implements the
> **Linux x86_64 syscall ABI** (numbers, semantics, `-errno` returns, struct layouts). No Linux code is used.
> Source of truth for numbers: `arch/x86/entry/syscalls/syscall_64.tbl` from the archived Linux source release.
> Never invent a number. Unimplemented syscalls return `-ENOSYS`.

## Calling Convention (x86_64)

- Entry: `SYSCALL`; return: `SYSRET`
- Number: `RAX`; arguments: `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9`
- Return: `RAX`; values in `[-4095, -1]` are `-errno`
- Clobbered by hardware: `RCX`, `R11`
- `brk` is the exception to `-errno`: it returns the (possibly unchanged) break

## Status

Status values: **done** (Linux number + semantics + test), **legacy** (works but still on the old arc_os number or
semantics), **stub** (returns `-ENOSYS` or partial), **missing**.

As of 2026-11-07 every existing syscall is **legacy**: the kernel still uses arc_os's own numbering in
`kernel/arch/x86_64/syscall.h`. The migration map is in ADR-0001. Update this table in the same PR that changes
a syscall (the `add-syscall` skill does this).

| Linux # | Name | Status | Old arc_os # | Needed by |
|---|---|---|---|---|
| 0 | read | legacy | 4 | all |
| 1 | write | legacy | 1 | all |
| 2 | open | legacy | 3 | libarc |
| 3 | close | legacy | 5 | all |
| 4 | stat | legacy | 8 | libarc |
| 5 | fstat | legacy | 12 | musl |
| 8 | lseek | legacy | 7 | all |
| 9 | mmap | missing | — | musl, llama.cpp (M1) |
| 10 | mprotect | missing | — | musl (M1) |
| 11 | munmap | missing | — | musl (M1) |
| 12 | brk | legacy | 6 | musl |
| 13 | rt_sigaction | missing (old `signal` = 20) | 20 | musl |
| 14 | rt_sigprocmask | missing | — | musl threads (M1) |
| 15 | rt_sigreturn | legacy | 22 | musl |
| 16 | ioctl | missing (old `tcsetpgrp` = 33) | 33 | shell, fbdev, input |
| 22 | pipe | legacy | 19 | shell |
| 28 | madvise | missing | — | llama.cpp (M1) |
| 32 | dup | legacy | 13 | shell |
| 33 | dup2 | legacy | 14 | shell |
| 35 | nanosleep | missing | — | M1 |
| 39 | getpid | legacy | 2 | all |
| 41 | socket | legacy | 35 | net, arc-llm |
| 42 | connect | legacy | 39 | net, arc-llm |
| 43 | accept | legacy | 38 | net, arc-llm |
| 44 | sendto | legacy | 40, 42 | net |
| 45 | recvfrom | legacy | 41, 43 | net |
| 49 | bind | legacy | 36 | net |
| 50 | listen | legacy | 37 | net |
| 56 | clone | missing | — | musl threads (M1) |
| 57 | fork | legacy | 16 | shell |
| 59 | execve | legacy (no envp) | 17 | shell |
| 60 | exit | legacy | 0 | all |
| 61 | wait4 | legacy (old `wait`) | 18 | shell |
| 62 | kill | legacy | 21 | shell |
| 79 | getcwd | legacy | 24 | shell |
| 80 | chdir | legacy | 23 | shell |
| 83 | mkdir | legacy | 9 | coreutils |
| 87 | unlink | legacy | 11 | coreutils |
| 90 | chmod | legacy | 29 | coreutils |
| 92 | chown | legacy | 30 | coreutils |
| 95 | umask | legacy | 34 | shell |
| 102 | getuid | legacy | 25 | login |
| 104 | getgid | legacy | 26 | login |
| 105 | setuid | legacy | 27 | login |
| 106 | setgid | legacy | 28 | login |
| 109 | setpgid | legacy | 31 | shell |
| 110 | getppid | legacy | 15 | shell |
| 121 | getpgid | legacy | 32 | shell |
| 158 | arch_prctl | missing | — | musl TLS (M1) |
| 202 | futex | missing | — | musl threads (M1) |
| 203 | sched_setaffinity | missing | — | P-core pinning (M1) |
| 217 | getdents64 | missing (old `readdir` = 10) | 10 | musl, coreutils |
| 218 | set_tid_address | missing | — | musl (M1) |
| 228 | clock_gettime | missing | — | M1 |
| 231 | exit_group | missing | — | musl |
| 257 | openat | missing | — | musl |
| 262 | newfstatat | missing | — | musl |
| 293 | pipe2 | missing | — | musl |

Numbers above are from memory; verify each against the archived `syscall_64.tbl` when it is migrated.
The full list needed by M1/M2 comes from the M0 syscall inventory (strace of llama.cpp, SQLite and our test
programs on Linux); add rows for anything it finds.

## Argument Validation

Every pointer argument from user space MUST be validated before dereference:
1. Pointer is below `USER_ADDR_LIMIT` (`0x00007FFFFFFFFFFF`)
2. The range is mapped and accessible (or fault-handled via copy_from_user/copy_to_user)
3. `ptr + len` does not overflow into kernel space
