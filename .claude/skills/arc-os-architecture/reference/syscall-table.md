# Syscall Table Reference

> Binding decision: [ADR-0001](../../../../docs/decisions/0001-linux-syscall-abi.md) — arc_os implements the
> **Linux x86_64 syscall ABI** (numbers, semantics, `-errno` returns, struct layouts). No Linux code is used.
> Source of truth for numbers: `arch/x86/entry/syscalls/syscall_64.tbl` from the archived Linux source release.
> Never invent a number. Unimplemented syscalls return `-ENOSYS`.

## Calling Convention (x86_64)

- Entry: `SYSCALL`; return: `SYSRET`
- Number: `RAX`; arguments: `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9`
- Return: `RAX`; values in `[-4095, -1]` are `-errno`
- Clobbered by hardware: `RCX`, `R11`; every other register is preserved (musl's inline syscalls rely on it;
  before M0-06 arc_os clobbered `RDI`–`R10`)
- `brk` is the exception to `-errno`: it returns the (possibly unchanged) break

## Status

Status values: **done** (Linux number + semantics + test), **partial** (Linux number and core semantics; the
gap is named in brackets), **legacy** (works but still on the old arc_os number or semantics), **stub** (returns
`-ENOSYS`), **missing**.

Since M0-05 (2026-11-08) every implemented syscall uses its Linux number (`kernel/include/uapi/unistd.h`) and
nothing is **legacy**; the old numbers are gone. Handlers live in `kernel/{fs,proc,mm,net}/*_syscalls.c`. Unknown
or unimplemented numbers return `-ENOSYS`. Raw-errno behaviour is checked in QEMU by `qemu.abi`
(`userland/testprogs/abi_check.c`), struct layouts on the host by `test_abi_layout`. Update this table in the
same PR that changes a syscall (the `add-syscall` skill does this). Since M0-06 stock static musl programs
(ET_EXEC and static-PIE) start and run: `qemu.musl` (`tests/target/start_basic.c`).

| Linux # | Name | Status | Old arc_os # | Needed by |
|---|---|---|---|---|
| 0 | read | done | 4 | all; inventory: musl, C++, SQLite, llama.cpp |
| 1 | write | done | 1 | all; inventory: musl, C++ |
| 2 | open | done (symlinks and O_NOFOLLOW since M1-13) | 3 | libarc; inventory: musl, C++, SQLite, zstd, llama.cpp (M0-05, M1-11) |
| 3 | close | done | 5 | all; inventory: musl, C++, SQLite, zstd, llama.cpp |
| 4 | stat | done | 8 | libarc; inventory: musl, C++, SQLite, zstd, llama.cpp (M0-05, M1-11) |
| 5 | fstat | done | 12 | musl; inventory: musl, C++, SQLite, zstd (M0-05, M1-11) |
| 6 | lstat | done | — | inventory: SQLite (M1-11, M1-13) |
| 8 | lseek | done | 7 | all; inventory: musl, SQLite, llama.cpp |
| 9 | mmap | partial (anonymous private: MAP_FIXED, MAP_FIXED_NOREPLACE, MAP_NORESERVE, MAP_POPULATE, MAP_32BIT, MAP_GROWSDOWN; files in the page cache (ramfs, FAT32), MAP_PRIVATE and MAP_SHARED, through one cache with read/write (M1-12): EACCES rules as Linux, -ENODEV for files without mmap (directories, devices until M1-15); MAP_32BIT ignored for files; MAP_SHARED anonymous -EINVAL until M1-08) | — | musl, llama.cpp; inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M1-07, M1-12) |
| 10 | mprotect | partial (a range with a hole fails whole with -ENOMEM; Linux changes the part before it; PROT_GROWSDOWN/UP -EINVAL) | — | musl; inventory: musl, musl threads, C++, zstd, llama.cpp (M1-07) |
| 11 | munmap | done (splits VMAs, frees pages and emptied page tables after the TLB flush, M1-07) | — | musl; inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M1-07) |
| 12 | brk | done (a [heap] VMA, pages on first touch; one free page kept above, M1-07) | 6 | musl; inventory: musl, C++, SQLite, zstd, llama.cpp (M1-07) |
| 13 | rt_sigaction | partial (SA_RESTORER, SA_NODEFER, SA_RESETHAND and sa_mask act; SA_RESTART, SA_ONSTACK, SA_NOCLD* stored, act in M1-17) | 20 | musl; inventory: musl, SQLite, zstd, llama.cpp (M1-01) |
| 14 | rt_sigprocmask | missing | — | musl threads; inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M1-01) |
| 15 | rt_sigreturn | done (Linux rt_sigframe with XSAVE image, M1-01) | 22 | musl; inventory: musl (M1-01) |
| 16 | ioctl | partial (terminal only: TIOCS/GPGRP, TCGETS, TCSETS/W/F, TIOCG/SWINSZ; termios stored, not applied until M5) | 33 | shell, fbdev, input; inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M0-06) |
| 17 | pread64 | done | — | inventory: musl, SQLite (M1-11) |
| 18 | pwrite64 | done | — | inventory: SQLite (M1-11) |
| 19 | readv | done | — | inventory: zstd, llama.cpp (M0-06) |
| 20 | writev | done | — | inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M0-06) |
| 21 | access | missing | — | inventory: musl, SQLite (M1-20) |
| 22 | pipe | done | 19 | shell; inventory: musl |
| 25 | mremap | partial (MREMAP_MAYMOVE, MREMAP_FIXED; MREMAP_DONTUNMAP and old_len 0 -EINVAL) | — | musl realloc; inventory: musl (M1-07) |
| 26 | msync | done (MS_SYNC writes the range's shared file pages back and waits; MS_ASYNC starts nothing, as on Linux; MS_INVALIDATE has nothing to drop) | — | SQLite, llama.cpp (M1-12) |
| 28 | madvise | partial (MADV_DONTNEED and MADV_FREE drop the pages now; NORMAL, RANDOM, SEQUENTIAL set file readahead, WILLNEED reads file pages in now (M1-12); HUGEPAGE, NOHUGEPAGE accepted, no effect yet) | — | llama.cpp; inventory: llama.cpp (M1-07, M1-12) |
| 32 | dup | done | 13 | shell; inventory: musl |
| 33 | dup2 | done | 14 | shell |
| 35 | nanosleep | missing | — | M1; inventory: musl, C++ (M1-06) |
| 39 | getpid | done | 2 | all; inventory: musl, SQLite |
| 41 | socket | partial (AF_INET only; SOCK_NONBLOCK/CLOEXEC ignored) | 35 | net, arc-llm (M1-18: AF_UNIX, SOCK_CLOEXEC) |
| 42 | connect | done | 39 | net, arc-llm (M1-18: AF_UNIX) |
| 43 | accept | done | 38 | net |
| 44 | sendto | partial (flags ignored) | 40, 42 | net |
| 45 | recvfrom | partial (flags ignored) | 41, 43 | net, arc-llm (M1-18: AF_UNIX) |
| 46 | sendmsg | missing | — | arc-llm: iovec, MSG_NOSIGNAL (M1-18) |
| 48 | shutdown | missing | — | arc-llm: SHUT_RDWR on a listening socket fails accept (M1-18) |
| 49 | bind | done | 36 | net, arc-llm (M1-18: AF_UNIX path) |
| 50 | listen | done | 37 | net, arc-llm (M1-18: AF_UNIX) |
| 54 | setsockopt | missing | — | arc-llm: SO_RCVTIMEO, SO_SNDTIMEO (M1-18) |
| 56 | clone | missing | — | musl threads; inventory: musl threads, C++, zstd, llama.cpp (M1-16) |
| 57 | fork | done | 16 | shell; inventory: musl (M0-05) |
| 59 | execve | done (System V stack + auxv, ET_EXEC and static-PIE; AT_SECURE always 0 until poll, M1-18) | 17 | shell; inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M0-06) |
| 60 | exit | done | 0 | all; inventory: musl threads, C++, zstd, llama.cpp (M1-16) |
| 61 | wait4 | partial (rusage zero-filled; WCONTINUED never reported) | 18 | shell; inventory: musl (M0-05) |
| 62 | kill | partial (pid -1 and signal 0 not supported) | 21 | shell |
| 72 | fcntl | partial (F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD/SETFD, F_GETFL/SETFL; record locks M1-20) | — | inventory: musl, C++, SQLite (M1-11, M1-20) |
| 74 | fsync | partial (writes the file's dirty page-cache pages back (M1-12); ramfs and read-only FAT32 have nowhere to write; disk write-back M1-15) | — | inventory: musl, SQLite (M1-11, M1-12, M1-15) |
| 75 | fdatasync | partial (same as fsync: no metadata is kept apart from the data) | — | SQLite (M1-12) |
| 77 | ftruncate | done | — | inventory: musl (M1-11) |
| 79 | getcwd | done (from the cwd's inode since M1-13: follows renames) | 24 | shell; inventory: musl, SQLite |
| 80 | chdir | done | 23 | shell; inventory: musl |
| 82 | rename | done | — | inventory: musl, C++ (M1-11) |
| 83 | mkdir | done | 9 | coreutils; inventory: musl, C++ |
| 84 | rmdir | done | — | inventory: musl, C++ (M1-11) |
| 87 | unlink | done | 11 | coreutils; inventory: musl, C++, SQLite |
| 89 | readlink | done | — | symlinks (M1-13) |
| 90 | chmod | done | 29 | coreutils; inventory: musl |
| 91 | fchmod | missing | — | inventory: zstd (M1-20) |
| 92 | chown | done | 30 | coreutils |
| 93 | fchown | missing | — | inventory: SQLite, zstd (M1-20) |
| 95 | umask | done | 34 | shell |
| 99 | sysinfo | missing | — | inventory: llama.cpp (M1-20) |
| 102 | getuid | done | 25 | login; inventory: musl, SQLite |
| 104 | getgid | done | 26 | login |
| 105 | setuid | partial (root only) | 27 | login |
| 106 | setgid | partial (root only) | 28 | login |
| 107 | geteuid | missing | — | inventory: musl, SQLite (M1-20) |
| 109 | setpgid | partial (no permission checks) | 31 | shell |
| 110 | getppid | done | 15 | shell; inventory: musl |
| 121 | getpgid | done | 32 | shell |
| 158 | arch_prctl | partial (ARCH_SET_FS / ARCH_GET_FS only) | — | musl TLS; inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M0-06) |
| 162 | sync | done (writes every mapping waiting for write-back and waits, a flusher pass already running included; M1-14) | — | write-back tests, shutdown (M1-14) |
| 200 | tkill | missing | — | inventory: musl (M1-17) |
| 202 | futex | missing | — | musl threads; inventory: musl threads, C++, zstd, llama.cpp (M1-16) |
| 203 | sched_setaffinity | missing | — | P-core pinning (M1) |
| 204 | sched_getaffinity | missing | — | inventory: C++, llama.cpp (M1-17) |
| 217 | getdents64 | done | 10 | musl, coreutils; inventory: musl, C++ (M0-05, M1-11) |
| 218 | set_tid_address | partial (pointer stored; cleared on thread exit from M1-16) | — | musl; inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M0-06) |
| 221 | fadvise64 | missing | — | inventory: llama.cpp (M1-12) |
| 228 | clock_gettime | missing | — | M1; inventory: zstd (M1-06) |
| 231 | exit_group | partial (= exit while a process has one thread; M1-16) | — | musl; inventory: musl, musl threads, C++, SQLite, zstd, llama.cpp (M0-06) |
| 257 | openat | done (a dirfd is followed by inode since M1-13) | — | musl; inventory: C++ (M0-05, M1-11) |
| 258 | mkdirat | done | — | musl (M1-11) |
| 262 | newfstatat | done | — | musl (M1-11, M1-13) |
| 263 | unlinkat | done | — | inventory: C++ (M1-11) |
| 264 | renameat | done | — | musl (M1-11) |
| 267 | readlinkat | done | — | musl `readlink` (M1-13) |
| 280 | utimensat | missing | — | inventory: zstd (M1-20) |
| 288 | accept4 | missing | — | arc-llm: AF_UNIX, SOCK_CLOEXEC, EINTR without SA_RESTART (M1-18) |
| 292 | dup3 | done | — | musl (M1-11) |
| 293 | pipe2 | done | — | musl (M1-11) |

Numbers verified against the archived Linux v6.12 `syscall_64.tbl` (M0-04). "inventory:" lists the programs
that made the call in the M0-04 syscall inventory (`docs/plans/syscall-inventory.md`, musl static builds under
strace) and, in brackets, the block that owns it. musl reads the clock through the vDSO, so strace never sees
`clock_gettime`/`gettimeofday` from most programs; arc_os has no vDSO, so musl will make them as syscalls.

## Argument Validation

Every pointer argument from user space MUST be validated before dereference:
1. Pointer is below `USER_ADDR_LIMIT` (`0x00007FFFFFFFFFFF`)
2. The range is mapped and accessible (or fault-handled via copy_from_user/copy_to_user)
3. `ptr + len` does not overflow into kernel space
