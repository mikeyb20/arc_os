# ADR-0001: Linux x86_64 syscall ABI

- **Status:** Accepted (Michael, 2026-11-07)
- **Date:** 2026-11-07

## Context
arc_os had its own syscall numbering (exit=0, write=1, getpid=2, open=3, ...) which collides with Linux
(read=0, write=1, open=2, ...). We must port musl, a C++ runtime, llama.cpp and SQLite, and eventually
install other programs. Michael wants independence from Linux as a project, but tools that also work on Linux.

## Decision
arc_os implements the **Linux x86_64 syscall ABI as a copied convention**: syscall numbers, argument order,
semantics, `-errno` return convention, and struct layouts (`stat`, `dirent64`, `timespec`, `sigaction`,
`input_event`, fbdev structs, ...). No Linux source code is used. Entry stays `SYSCALL`/`SYSRET`
(number in RAX; args in RDI, RSI, RDX, R10, R8, R9; RCX/R11 clobbered).

Source of truth for numbers: `arch/x86/entry/syscalls/syscall_64.tbl` from an archived Linux source release
(pinned version recorded in the syscall inventory). Never invent a number; unimplemented calls return `-ENOSYS`.

Migration of today's 44 syscalls (done in one pass with libarc, userland and tests):

| arc_os today | Linux x86_64 target | Note |
|---|---|---|
| 0 EXIT | 60 `exit` (+231 `exit_group`) | |
| 1 WRITE | 1 `write` | |
| 2 GETPID | 39 `getpid` | |
| 3 OPEN | 2 `open` (+257 `openat`) | musl uses `openat` |
| 4 READ | 0 `read` | |
| 5 CLOSE | 3 `close` | |
| 6 BRK | 12 `brk` | Linux semantics: returns new break, never -errno |
| 7 LSEEK | 8 `lseek` | |
| 8 STAT | 4 `stat` (+262 `newfstatat`) | Linux `struct stat` layout |
| 9 MKDIR | 83 `mkdir` | |
| 10 READDIR | 217 `getdents64` | different semantics: buffer of `linux_dirent64` |
| 11 UNLINK | 87 `unlink` | |
| 12 FSTAT | 5 `fstat` | |
| 13 DUP | 32 `dup` | |
| 14 DUP2 | 33 `dup2` | |
| 15 GETPPID | 110 `getppid` | |
| 16 FORK | 57 `fork` / 56 `clone` | musl uses `clone` |
| 17 EXEC | 59 `execve` | takes `envp` |
| 18 WAIT | 61 `wait4` | status encoding per Linux |
| 19 PIPE | 22 `pipe` (+293 `pipe2`) | |
| 20 SIGNAL | 13 `rt_sigaction` | `signal()` becomes a libc wrapper |
| 21 KILL | 62 `kill` | |
| 22 SIGRETURN | 15 `rt_sigreturn` | |
| 23 CHDIR | 80 `chdir` | |
| 24 GETCWD | 79 `getcwd` | |
| 25 GETUID | 102 `getuid` | |
| 26 GETGID | 104 `getgid` | |
| 27 SETUID | 105 `setuid` | |
| 28 SETGID | 106 `setgid` | |
| 29 CHMOD | 90 `chmod` | |
| 30 CHOWN | 92 `chown` | |
| 31 SETPGID | 109 `setpgid` | |
| 32 GETPGID | 121 `getpgid` | |
| 33 TCSETPGRP | 16 `ioctl(TIOCSPGRP)` | becomes an ioctl |
| 34 UMASK | 95 `umask` | |
| 35 SOCKET | 41 `socket` | |
| 36 BIND | 49 `bind` | |
| 37 LISTEN | 50 `listen` | |
| 38 ACCEPT | 43 `accept` | |
| 39 CONNECT | 42 `connect` | |
| 40 SEND | 44 `sendto` | `send()` becomes a libc wrapper |
| 41 RECV | 45 `recvfrom` | `recv()` becomes a libc wrapper |
| 42 SENDTO | 44 `sendto` | |
| 43 RECVFROM | 45 `recvfrom` | |

Numbers above are from memory and **must be checked against the archived `syscall_64.tbl`** during the
M0 migration.

## Consequences
- musl ports with few changes; a stock `x86_64-linux-musl` toolchain builds our user space.
- Static binaries built for arc_os run on Linux and vice versa (as syscalls are implemented).
- arc_os's own numbering scheme and reserved ranges are retired.
- Rules out: arc_os-specific syscall numbers. arc_os-only features go through `ioctl` on a device, or a
  new syscall above Linux's range only with a new ADR.

## Verification
- Host tests per syscall; a QEMU test that runs a static musl "hello" and a libarc "hello".
- M0 syscall inventory (strace on Linux) lists every syscall M1/M2 need; each is tracked as done/stub/missing in
  `.claude/skills/arc-os-architecture/reference/syscall-table.md`.
