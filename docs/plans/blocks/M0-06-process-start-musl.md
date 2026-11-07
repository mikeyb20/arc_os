# M0-06 — Linux process start and a musl hello world

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M0-05 | M1-01, M1-03, M1-11 |

**Goal.** A static program built with stock musl starts and runs on arc_os exactly as on Linux (ADR-0002).
This block closes M0.

**Read first.**
- `docs/decisions/0002-process-start-and-tls.md`, the musl startup rows of `docs/plans/syscall-inventory.md`
- `kernel/proc/elf.c`, `kernel/proc/init.c`, the `execve` handler (moved in M0-05), `jump_to_usermode`,
  `kernel/arch/x86_64/context_switch.asm`, `kernel/proc/thread.h`
- `libc/src/crt0.c`, `kernel/include/user_access.h`

**Build.**
1. `execve`: copy argv and envp in with `user_access.h` helpers (no direct user-pointer reads; today's
   `exec_copy_argv` reads `user_argv[i]` directly), with a total size limit. Build the System V initial stack:
   argc, argv, NULL, envp, NULL, auxv (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_BASE`=0,
   `AT_FLAGS`=0, `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`, `AT_SECURE`=0, `AT_RANDOM` (16 bytes),
   `AT_HWCAP`, `AT_EXECFN`, `AT_PLATFORM`, `AT_NULL`). RSP points at argc and is 16-byte aligned; RDX = 0.
2. ELF loader: `ET_EXEC` and static-PIE (`ET_DYN` without `PT_INTERP`, loaded at a fixed base); `PT_LOAD`
   permissions; `PT_GNU_STACK`. Programs with `PT_INTERP` get `-ENOEXEC` and a log line (static only, ADR-0002).
3. libarc `crt0`: read argc, argv, envp and auxv from the stack; add `environ`, `getenv`, `getauxval`.
4. TLS: `arch_prctl(ARCH_SET_FS / ARCH_GET_FS)`; the FS base is saved and restored per thread on context
   switch (MSR for now) and copied by fork.
5. `set_tid_address` (store the pointer, return the tid), `exit_group` (same as exit until M1-16),
   `writev`/`readv`, tty `ioctl`: `TCGETS`, `TCSETS`, `TCSETSW`, `TIOCGWINSZ`, `TIOCSWINSZ` (plus the
   pgrp ones from M0-05); other files return `-ENOTTY`.
6. Build `tests/target/libc_basic.c` twice, with Ubuntu's `musl-gcc -static` and with libarc. QEMU suite `musl`:
   argv/envp/auxv checks, printf, file I/O, getenv, exit status. No signals and no malloc yet (see Watch out).

**Done when.**
- [ ] `ctest -L qemu -R musl` passes, and libarc programs still pass.
- [ ] `syscall-table.md` rows updated (`arch_prctl`, `set_tid_address`, `exit_group`, `writev`, `readv`, `ioctl`).
- [ ] M0 boxes in knowledge-os.md §8 ticked (M0 exit check).

**Watch out.**
- musl's malloc needs `mmap` (M1-07). Keep M0 tests malloc-free (static buffers).
- musl's signal handlers need the Linux signal frame (M1-01). Keep signal handlers out of M0 musl tests.
- Missing `AT_UID`/`AT_EUID`/`AT_SECURE` makes musl take its secure-mode path (it polls fds 0–2 at start).

**Notes.**
-
