# M1-05 — SMP scheduler

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | K | M1-03 | M1-06, M1-07, M1-09, M1-16 |

**Goal.** All CPUs run user and kernel threads from per-CPU run queues, with balancing, affinity masks and
written lock ordering that debug builds check at runtime.

**Read first.**
- `kernel/proc/sched.c`, `sched.h`, `thread.c`, `thread.h`, `process.c`, `spinlock.h`, `waitqueue.c`, `mutex.c`
- `kernel/arch/x86_64/smp.c`, `ipi.c`, `lapic.c`
- `docs/plans/work-blocks.md` §7 (lock order), Notes of M1-03

**Build.**
1. Per-CPU run queue (IRQ-safe spinlock) with today's policy (round robin with time slices); one idle thread per
   CPU (HLT now, MWAIT in M1-21). The global `sched_lock` goes away.
2. Application CPUs enter the scheduler after init. A wakeup picks the target CPU (last CPU if allowed and idle,
   else the least loaded allowed CPU) and sends a reschedule IPI when needed.
3. Affinity mask per thread (`MAX_CPUS` bits). P-core and E-core masks come from the per-CPU core type.
4. Balancing: an idle CPU pulls from the busiest queue. No fixed periodic balancing tick (ADR-0017).
5. Migration rules: a running thread is never changed remotely; when two run queues must be locked, lock them in
   CPU-index order.
6. Lock levels: every spinlock and sleeping lock gets a level from §7. Debug builds panic on an out-of-order
   acquisition or on sleeping while holding a spinlock. Write `docs/kernel/locking.md` from §7.
7. Per-CPU statistics in `/proc` (context switches, run-queue length, idle time).
8. Tests (QEMU `-smp 4`, `-accel tcg,thread=multi`): 64 CPU-bound processes spread over all CPUs; fork/exec/wait
   10 000 times with no leak (free page count returns to its baseline); pipe ping-pong across CPUs; the lock
   checker catches a deliberately inverted pair (test-only).

**Done when.**
- [ ] Tests pass in TCG and in CI.
- [ ] `docs/kernel/locking.md` exists and matches what the checker enforces.

**Watch out.**
- Wait queues and mutexes must take the target CPU's run-queue lock when waking a thread.
- A migrated single-threaded process reloads CR3, so no TLB shootdown is needed here (M1-07 adds shootdown).
  Don't add PCID yet.
- TCG runs all vCPUs on one host thread unless `thread=multi` is set, which hides races.
- L block: if the session runs long, stop after per-CPU queues + AP scheduling pass their tests, and hand off.

**Notes.**
-
