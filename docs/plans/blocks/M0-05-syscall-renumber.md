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
- [ ] `grep -rn "SYS_" userland/ libc/ kernel/` finds nothing except the uapi `__NR_` header and its users.
- [ ] Host tests, `test_abi_layout`, and `ctest -L qemu` (shell and coreutils) pass.
- [ ] `syscall-table.md` updated.

**Watch out.**
- Linux `brk` returns the new break, never `-errno`.
- `getdents64` fills as many records as fit; `d_reclen` is 8-byte aligned; `d_off` is an opaque cookie.
- `wait4` status bits must match Linux (`WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED`).
- Keep behaviour changes to what the renumbering needs. Process start and new syscalls are M0-06.

**Notes.**
-
