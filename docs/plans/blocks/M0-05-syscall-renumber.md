# M0-05 — Move syscalls to Linux numbers

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M0-02, M0-04 | M0-06 |

**Goal.** Every existing syscall uses its Linux x86_64 number and calling convention. libarc, userland and tests
change in the same PR, and no old number is left anywhere (ADR-0001).

**Read first.**
- `docs/decisions/0001-linux-syscall-abi.md` (migration table), `syscall-table.md`,
  `docs/plans/syscall-inventory.md`
- `kernel/arch/x86_64/syscall.h`, `kernel/arch/x86_64/syscall.c` (dispatch and registration only; open handler
  bodies when you move them)
- `libc/include/syscall.h`, `libc/src/unistd.c`, `stat.c`, `dirent.c`, `signal.c`, `wait.c`
- Direct `SYS_` users: `userland/shell/shell.c`, `userland/coreutils/ls.c`, `ps.c`, `mv.c`,
  `userland/login/login.c`
- `tests/test_syscall.c`
- `.claude/skills/add-syscall/SKILL.md`

**Build.**
1. `kernel/include/uapi/` (our own code, shared by the kernel and libarc): `__NR_*` numbers, Linux errno values,
   and the structs that cross the boundary (`stat`, `linux_dirent64`, `timespec`, kernel `sigaction`, `iovec`,
   `winsize`, `termios`, `pollfd`, `sockaddr_in`, `sockaddr_un`), each with `_Static_assert`s on size and key
   offsets.
2. Dispatch table sized 512; unknown numbers return `-ENOSYS`.
3. Renumber the one-to-one calls. Convert the rest: `readdir` → `getdents64`; `wait` → `wait4`;
   `signal` → `rt_sigaction` (kernel `sigaction` layout, `sigsetsize` must be 8; keep the existing signal frame,
   M1-01 replaces it); `tcsetpgrp`/`tcgetpgrp` → `ioctl(TIOCSPGRP/TIOCGPGRP)`; `send`/`recv` →
   `sendto`/`recvfrom`; `exec` → `execve` (envp may be ignored until M0-06).
4. Move every handler you touch from `syscall.c` into its subsystem (`fs/`, `proc/`, `mm/`, `net/`).
   `syscall.c` keeps only dispatch and registration.
5. libarc wrappers on `__NR_*`, setting `errno` from negative returns. Userland: replace every direct `SYS_` use
   with a libarc call. Delete the old `SYS_*` headers.
6. `tests/test_abi_layout.c`: built with the host compiler and the host's Linux headers, compares `sizeof` and
   `offsetof` of our uapi structs with the host's.
7. Update the status column in `syscall-table.md`.

**Done when.**
- [x] `grep -rn "SYS_" userland/ libc/ kernel/` finds nothing except the uapi `__NR_` header and its users.
- [x] Host tests, `test_abi_layout`, and `ctest -L qemu` (shell and coreutils) pass.
- [x] `syscall-table.md` updated.

**Watch out.**
- Linux `brk` returns the new break, never `-errno`.
- `getdents64` fills as many records as fit; `d_reclen` is 8-byte aligned; `d_off` is an opaque cookie.
- `wait4` status bits must match Linux (`WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED`).
- Keep behaviour changes to what the renumbering needs. Process start and new syscalls are M0-06.

**Notes.**
- From M0-02: `sys_wait` now takes libc's (pid, status, options) order but ignores pid (reaps any child);
  `wait4` must honour pid. Run `ctest -L qemu` (smoke + userland cover the shell and coreutils).
- 2026-11-08 (M0-05 PR): layout. Numbers in `kernel/include/uapi/unistd.h` (78, generated from the archived tbl,
  table size `ARC_NR_SYSCALLS` = 512); errno, fcntl, stat, dirent, time, signal, uio, termios, poll, socket and
  wait headers beside it, reached as `<uapi/...>` through `-idirafter kernel/include` (libarc, host tests).
  Handlers: `kernel/{fs,proc,mm,net}/*_syscalls.c`, each with an `*_syscalls_init()` that `syscall_init` calls.
  libarc's raw helpers are private (`libc/src/syscall_arch.h`); `libc/include/syscall.h` is gone.
- Tests added: `test_abi_layout` (283 numbers, constants, sizes and offsets vs the host's Linux uapi and glibc
  headers; `tests/abi_layout_list.h` is the list), `qemu.abi` (`userland/testprogs/abi_check.c`: raw `-errno`
  checks for getdents64, stat, wait4, rt_sigaction, ioctl, brk, getcwd, sockets, `-ENOSYS`), and `qemu.smoke` now
  runs shell builtins (ls, stat, cd, pwd, run, id) from `smoke.sh`.
- Kernel bugs found by those tests and fixed here (all older than M0-05):
  - `proc_fork` passed the child's context through one global, so two forks before the first child ran started
    both children with the second fork's registers and address space. Now a kmalloc'd copy per child.
  - Signal handlers could never return: RSP on entry pointed at the trampoline bytes, not at the return address.
    The arc_os frame is kept (M1-01 replaces it) but placed correctly, below the red zone and ABI-aligned;
    `rt_sigreturn` finds it at RSP - sizeof(SignalFrame).
  - A process killed by a signal never woke a parent blocked in wait, sent no SIGCHLD and kept its fds (pipe
    readers never saw EOF). `sig_terminate` now does what `exit` does.
  - `brk` remapped already-mapped pages after a shrink or a partial failure (frame leak) and had no upper bound
    (a large value mapped user pages over the stack and into kernel addresses). Now: unmap on shrink, undo on
    failure, limit 1 MiB below the stack.
  - fds 0-2 had no fd-table entries (the kernel fell back to the TTY), so a process's first `open()` returned 0
    and `opendir` in the shell took fd 1: `ls` output went into a directory. init now gets `/dev/tty` on 0-2.
- Linux semantics adopted while renumbering: `getuid`/`getgid` return the real IDs; `wait4` reports stopped
  children only with `WUNTRACED` (the shell passes it) and `WIFSIGNALED` for signal deaths; `exit` keeps the low
  8 bits; bad user pointers give `-EFAULT`; zero-length read/write return 0; `getcwd` returns the length or
  `-ERANGE`; full fd table gives `-EMFILE`; directories open read-only (`O_DIRECTORY` checked), `read` on one
  gives `-EISDIR`; socket addresses need `addrlen >= 16` and `AF_INET`.
- Still partial (see syscall-table.md): rt_sigaction keeps only the handler; execve ignores envp (M0-06); ioctl
  knows TIOCSPGRP/TIOCGPGRP only; getdents64 re-lists the directory from the start on every call (O(offset), at
  most 256 entries, a 68 KiB kmalloc at worst) until M1-11; closing a socket fd does not free the socket (no
  refcount yet; M1-18).
- libarc and test programs must not zero structs with `= { 0 }` until M1-01 enables user SSE: GCC emits `pxor`/
  `movaps` and the program dies with #UD (hit in `signal()`; the add-syscall skill now says so).

