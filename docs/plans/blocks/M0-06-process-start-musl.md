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
- [x] `ctest -L qemu -R musl` passes, and libarc programs still pass.
- [x] `syscall-table.md` rows updated (`arch_prctl`, `set_tid_address`, `exit_group`, `writev`, `readv`, `ioctl`).
- [x] M0 boxes in knowledge-os.md §8 ticked (M0 exit check).

**Watch out.**
- musl's malloc needs `mmap` (M1-07). Keep M0 tests malloc-free (static buffers).
- musl's signal handlers need the Linux signal frame (M1-01). Keep signal handlers out of M0 musl tests.
- Missing `AT_UID`/`AT_EUID`/`AT_SECURE` makes musl take its secure-mode path (it polls fds 0–2 at start).

**Notes.**

Done 2026-11-08.

*Scope change (Michael's OK, 2026-11-08).* Stock musl uses SSE before `main` (`__init_libc`, `__init_tls`,
printf; checked with objdump), so this block pulled the minimal part of M1-01 forward:
- the kernel is built with `-mgeneral-regs-only`;
- `fpu_cpu_init()` sets CR0.MP/NE, clears EM/TS, and sets CR4.OSFXSR/OSXMMEXCPT on every CPU;
- each thread has a 512-byte FXSAVE area holding a clean image (FCW 0x37F, MXCSR 0x1F80);
- `arch_thread_switch()` saves and restores it eagerly before `context_switch`;
- fork copies the live registers, and execve loads the clean image.

M1-01 swaps FXSAVE for XSAVE behind the same `kernel/arch/x86_64/fpu.h` calls. The M1-01 spec now says so.

*Where things are*
- `kernel/proc/exec_stack.c` builds the System V stack in a kernel buffer.
  - It is pure code, host-tested by the `exec_argv` suite.
  - From the top down: a zero word, the `AT_EXECFN` string, argv strings, envp strings, "x86_64", 16 `AT_RANDOM` bytes, then the pointer block with RSP 16-byte aligned at argc.
  - auxv holds `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`=0, `AT_FLAGS`=0, `AT_ENTRY`, `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`, `AT_SECURE`, `AT_RANDOM`, `AT_HWCAP`, `AT_HWCAP2`=0, `AT_EXECFN`, `AT_PLATFORM` and `AT_NULL`.
  - Limits: `EXEC_ARG_MAX` is 32 KiB of strings plus pointers, and `EXEC_MAX_PTRS` is 1024 entries; beyond either, execve returns -E2BIG.
- `proc_enter_user()` (`proc_syscalls.c`) is shared by execve and init.
  - It copies the stack in and resets the FS base and FPU, then calls `jump_to_usermode(entry, rsp)` with every register zero.
  - Init gets argv `{module path}` and envp `{HOME=/, TERM=linux}`, as on Linux.
- execve copies argv and envp with `copy_from_user`/`strncpy_from_user`.
  - These are new in `kernel/mm/uaccess.c` and check that every page is mapped for user access (`vmm_user_access_ok`). The kernel has no fault fixups yet (M1-19).
  - `fs_user_path` uses them too.
  - argv NULL or empty gives `argv[0] = ""`, as on Linux 5.18 and later.
  - Caught signal handlers are reset to SIG_DFL.
  - The old address space is freed after the CR3 switch; before, it was freed while still loaded.
- ELF loader (`elf.c`):
  - Accepts ET_EXEC and static-PIE (ET_DYN with no PT_INTERP), loaded at `ELF_PIE_BASE` 0x555555554000.
  - PT_INTERP gives -ENOEXEC plus a log line, and every malformed header gives -ENOEXEC (it was -EINVAL).
  - Checks overflow in phoff, offset and vaddr sums, plus filesz > memsz and phentsize.
  - AT_PHDR comes from PT_PHDR, or from the PT_LOAD whose file range holds the headers.
  - `libc/user.ld` now puts the headers in the first page so libarc programs have AT_PHDR.
- TLS: `Thread.fs_base`, `arch_prctl(ARCH_SET_FS/GET_FS)`. The scheduler loads MSR_FS_BASE only when it differs, keeping the invariant that the MSR equals the running thread's `fs_base`.
- `set_tid_address` stores `Thread.clear_child_tid` and returns the pid (tid == pid with one thread).
- `exit_group` is the same as `exit`.
- `readv`/`writev` loop over `sys_read`/`sys_write`: up to 1024 iovecs, stopping at the first short transfer or error.
- tty ioctls: `TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF` (flushes typed input), `TIOCGWINSZ` and `TIOCSWINSZ`.
  - termios is stored with Linux's defaults but not applied: input stays canonical with echo until M5.
  - The window size comes from the framebuffer console.
- libarc: `crt0.c` `_start` (asm) hands RSP to `__libc_start`, which sets `environ` and the auxv pointer. Added `getenv`, `getauxval` (`<sys/auxv.h>`); `execv` passes `environ`.

*Pre-existing bugs fixed*
1. **SYSCALL clobbered RDI, RSI, RDX, R8, R9 and R10** (RDI was set to the signal number, 0). Linux preserves every register except RAX, RCX and R11, and musl's inline syscalls rely on that: musl's stdio wrote garbage and faulted the kernel in `tty_write`. Fix: `syscall_entry.asm` saves and restores them, and fork children get them too (`ForkContext` grew 6 fields). Checked by `abi_check` `test_regs_preserved`.
2. **The ELF loader allocated a fresh page for a page two PT_LOAD segments share**, which dropped the first segment's bytes. Fix: it now reuses the page and merges permissions (W from either, X from either).
3. **execve leaked the new address space's pages on a load failure** (it used `vmm_destroy_user_pml4`). Fix: it now uses `vmm_free_user_pages`.
4. **execve kept caught signal handlers pointing into the old image.**

*Deviation from the spec.* `tests/target/libc_basic.c` stays the M1 target program: it uses malloc, signals, fork, time and rename, which this block can't support. The M0 test is `tests/target/start_basic.c`, built three ways:
- `start_musl`: `musl-gcc -static`
- `start_musl_pie`: `-static-pie`
- `start_arc`: libarc

`qemu.musl` runs all three with `alpha beta` and checks:
- argv, envp and environ
- every auxv entry
- RSP alignment
- printf/snprintf
- file I/O
- getenv
- for musl only: `__thread` (FS base), `isatty` (TIOCGWINSZ), tcgetattr/tcsetattr and writev

`abi_check` also checks:
- arch_prctl errors
- set_tid_address
- readv/writev edge cases
- termios ioctls
- execve -EFAULT, -E2BIG, -ENOENT and -ENOEXEC
- XMM7 and the FS base surviving pipe ping-pong between two processes. Removing the restore in `arch_thread_switch` makes this check fail; tried once.

*Known gaps, owned later*
- AT_SECURE is always 0. Linux sets it for setuid programs, but then musl calls `poll` on fds 0-2 and crashes if poll fails. poll arrives in M1-18.
- The stack stays executable (`stack_exec` is recorded but not applied) because the libarc signal trampoline lives on it. M1-01 fixes this with sa_restorer.
- A signal handler's return loses the interrupted code's argument registers and SSE state. M1-01 brings the Linux `rt_sigframe`.
- AT_RANDOM uses RDRAND when present, otherwise a TSC splitmix: fine for musl's canary, not for keys.
- The syscall entry still uses globals and one kernel stack. M1-03 makes them per-CPU.

*Block review (ADR-0020, run after master gained it)*
- The 8 new x86 includes this block added outside `kernel/arch/` now go through `arch/hal.h`, which includes `fpu.h` and `cpuid.h`. `hal.h`'s own comment had a `/*` inside a comment, so `-Werror` broke any file that included it; fixed. The count of direct includes outside arch equals master's.
- `cpu_random_fill` has its own `rdtsc`, as `security/hardening.c` does. Left alone (hardening.c is outside this block); M1-06's TSC clocksource is the natural shared helper.

*Costs (ADR-0019)*
- Per thread: 512 B more, plus FXSAVE/FXRSTOR (about 100 cycles each) on every switch; `wrmsr` only when FS bases differ.
- Per syscall: 6 pushes, 6 pops and 6 global stores.
- Per execve: two kmallocs (about 36 KiB and 37 KiB, freed before entering user mode) and one page walk per user page touched while copying arguments.
- Nothing is O(n) in a hot path.
