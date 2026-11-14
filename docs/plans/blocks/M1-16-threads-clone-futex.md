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
- From M1-05: process and signal state are covered by the interim big kernel lock; the thread-group and
  signal lock is level 70 (`LOCK_LEVEL_SIGNAL`). A thread that blocks gives the kernel lock up inside
  `sched_schedule` and takes it back after. `proc_reap` waits for the dead thread's last switch to finish
  (`sched_wait_off_cpu`) before freeing its stack; thread exit needs the same. fork copies the CPU affinity.
  Mutexes take a lock level (`mutex_init(m, level)`); debug builds check it. Threads sharing an mm need TLB
  shootdown on unmap (M1-07).
- From M1-07: `Process.mm` (`struct Mm`, `kernel/mm/mm.h`) replaces `page_table` and the brk fields. Threads
  share it with `mm_get` (`Mm.users`); the last `mm_put` frees it, after `mm_switch(NULL)` on the exiting CPU.
  `mm_switch` keeps `Mm.cpus` (who has it loaded) right, and `mm_flush_tlb_range` already IPIs every CPU in that
  set, so a second thread needs no new TLB code. The OOM killer and the `SIGKILL`-pending check in
  `mm_handle_fault` work per process; with threads, kill the whole group. Fault signals use
  `sig_force_fault` (per process today; per thread with you).
- From M1-13: the cwd is an inode reference (`Process.cwd`, `cwd_mnt`; `vfs_cwd_get/set/copy/release`) touched
  only by its own thread; `CLONE_FS` sharing needs a lock (or a shared fs struct) around it.
