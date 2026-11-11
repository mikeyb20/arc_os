# M1-16 — Threads I: thread groups, clone and basic futex

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | K | M1-01, M1-05, M1-07 | M1-17 |

**Goal.** A process can have many threads that share memory, files and signal handlers, created with Linux
`clone` flags and synchronised with futexes: enough for musl `pthread_create`, `pthread_join` and mutexes
(ADR-0002). Today a process holds exactly one thread.

**Read first.**
- `docs/decisions/0002-process-start-and-tls.md`, the `clone`/`futex` rows of `docs/plans/syscall-inventory.md`
- `kernel/proc/process.c`, `process.h`, `thread.c`, `thread.h`, `sched.c`, `signal.c`, `fd.c`
- Notes of M1-05 and M1-07

**Build.**
1. Split the process (thread group: mm, open files, cwd/umask, signal handlers, pid = tgid, children, exit state)
   from the thread (tid, registers, kernel stack with guard page, FS base, XSAVE area, signal mask, pending
   signals, `clear_child_tid`, affinity).
2. `clone` with `CLONE_VM`, `CLONE_FS`, `CLONE_FILES`, `CLONE_SIGHAND`, `CLONE_THREAD`, `CLONE_SYSVSEM` (accepted),
   `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`, `CLONE_DETACHED` (ignored),
   and `CLONE_VM | CLONE_VFORK` (musl's `posix_spawn` and `system()` use it: the parent sleeps until the child
   execs or exits). fork goes through the same path.
3. `gettid`, thread `exit` vs `exit_group`, `tkill`/`tgkill`; `wait4` sees the group leader as a zombie until every
   thread has exited.
4. `futex` `FUTEX_WAIT` and `FUTEX_WAKE`, private (keyed by mm + address) and shared (keyed by physical page +
   offset).
5. `clear_child_tid`: on thread exit, write 0 there and wake one futex waiter (this is how `pthread_join` works).
6. Tests (musl, `-smp 4`, `thread=multi`): create and join 4 to 64 threads; a contended mutex counter; `__thread`
   variables differ per thread; a thread exits while others run; `munmap` in one thread makes another thread's
   access fault (shootdown); `exit_group` from a non-main thread; `posix_spawn` runs a program.

**Done when.**
- [ ] Tests pass in TCG and in CI.

**Cost.** Futex wait/wake O(1) average through a hash keyed by (mm, address); per-thread overhead is the kernel stack, guard page, XSAVE area and a slab-allocated `struct thread`.

**Watch out.**
- Per-thread signals are M1-17. Until then, process-wide signals go to the main thread.
- The XSAVE area and FS base belong to the thread, not the process.
- L block: a clean stopping point is after steps 1–3 with single-thread behaviour unchanged and tests passing.

**Notes.**
- From M1-03: user GS base is always 0 (no `ARCH_SET_GS`), so it is not context-switched. If a thread ever gets
  its own GS base, save and restore MSR_KERNEL_GS_BASE (the user's value while in the kernel) on every switch.
