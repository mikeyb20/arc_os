# M1-05 — SMP scheduler

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | K | M1-03, M1-02b | M1-06, M1-07, M1-09, M1-16 |

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
6. Spinlock API: the saved interrupt flag moves out of the lock and into the caller
   (`spin_lock_irqsave(lock, &flags)` / `spin_unlock_irqrestore(lock, flags)`, plus plain `spin_lock` /
   `spin_unlock` for locks no interrupt handler takes). Today `proc/spinlock.h` keeps `saved_flags` inside the lock,
   so releasing two locks out of order turns interrupts back on while one is still held, and an interrupt that
   wants that lock deadlocks the CPU. Convert every caller; the `saved_flags` field goes. Host test: acquire A then
   B, release A then B, and interrupts stay off until the last release.
7. Lock levels: every spinlock and sleeping lock gets a level from §7. Debug builds panic on an out-of-order
   acquisition or on sleeping while holding a spinlock. Write `docs/kernel/locking.md` from §7.
8. Per-CPU statistics in `/proc` (context switches, run-queue length, idle time).
9. Tests (QEMU `-smp 4`, `-accel tcg,thread=multi`): 64 CPU-bound processes spread over all CPUs; fork/exec/wait
   10 000 times with no leak (free page count returns to its baseline); pipe ping-pong across CPUs; the lock
   checker catches a deliberately inverted pair (test-only).

**Done when.**
- [ ] Tests pass in TCG and in CI.
- [ ] `docs/kernel/locking.md` exists and matches what the checker enforces.
- [ ] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
  `arch/x86_64/`, and call the HAL through `hal_*` functions, not x86 names (work-blocks.md §7); the
  `arch_includes_outside_arch` limit in the budgets file is lowered to match (ADR-0020).

**Cost.** Picking the next thread O(1) from the local queue; choosing a wakeup CPU at most O(CPUs); no global lock in `schedule`; a context switch does no lookup by ID (thread points to its process and mm).

**Watch out.**
- Wait queues and mutexes must take the target CPU's run-queue lock when waking a thread.
- A migrated single-threaded process reloads CR3, so no TLB shootdown is needed here (M1-07 adds shootdown).
  Don't add PCID yet.
- TCG runs all vCPUs on one host thread unless `thread=multi` is set, which hides races.
- L block: if the session runs long, stop after per-CPU queues + AP scheduling pass their tests, and hand off.

**Notes.**
- From M0-02: `sched_yield()` no longer takes `sched_lock` (holding it across `context_switch` deadlocked);
  it only disables interrupts. Per-CPU queue locking replaces it here.
- From M1-03: `thread_current()` reads the per-CPU block through GS (`hal_current_thread`), and
  `hal_set_kernel_stack()` sets TSS.RSP0 and the SYSCALL stack of the CPU it runs on, so the switch path is already
  per CPU. APs sit in HLT on the bootloader's small stack (`ap_entry` in smp.c), with their GDT, TSS, IDT, GS,
  SYSCALL MSRs, XSAVE and APIC set up; give each its idle thread and own stack here. `percpu_get(i)` and
  `percpu_index_of_apic(id)` (O(log n)) map indexes and APIC IDs; `cpu_count` counts online CPUs. `kprintf` is not
  SMP-safe yet (interleaved lines when APs print). `MAX_CPUS` is 256: an affinity mask is 32 bytes.
- From M1-04: `kprintf` now appends to the kernel log ring (`lib/klog.c`) under an IRQ-safe leaf lock (§7 level
  10) and one console owner prints; writers never wait for a console. One `kprintf` call of up to 128 bytes lands
  in the ring in one piece; a line built from several calls can still have another CPU's message spliced into it.
  TTY/VT output still draws on the framebuffer without the console owner (`fb_console.c` header): put it under
  `klog_console_try_acquire` here, or the screen can tear when both draw. A panic stops the other CPUs with the
  ordinary halt IPI, so a CPU spinning with interrupts off keeps running: consider an NMI. The `arc.selftest`
  AVX2 check runs on the APs through `hal_run_on_other_cpus` (IPI 0xF3) while they idle; it relies on no user
  thread existing (diagnostic mode never starts init), which must stay true once APs run threads. `arc.bench`'s
  context-switch partner threads are never reaped (diagnostic mode only).
- From M1-05 (handoff): every CPU schedules from its own run queue; the boot thread is CPU 0's idle thread.
  Process, VFS, fd and signal state still had no locks, so an interim **big kernel lock** (level 0,
  `proc/kernel_lock.h`) is held from syscall entry to exit and by the keyboard and virtio-net IRQ handlers;
  it is dropped across `sched_schedule` and retaken after. User code runs in parallel, kernel code mostly
  doesn't yet. M1-07, M1-11 and M1-16 shrink it (`docs/kernel/locking.md` lists what it covers).
- Slice timer: LAPIC periodic 100 ms per CPU, armed only while a non-idle thread runs (3 APIC writes per such
  switch); the PIT now only counts uptime. M1-06 replaces both with per-CPU one-shot timers.
- Debug builds (`ARC_DEBUG=1`, the default) run the lock-order checker on every acquisition; per-CPU held
  lists live in the run queue, so locks taken before `sched_init` aren't tracked. Release builds compile it out.
- Not done: the panic still stops other CPUs with the ordinary halt IPI, not an NMI (a CPU spinning with
  interrupts off keeps running); `arc.selftest`'s AVX2 check still relies on diagnostic mode never starting init.
- Independent review (ADR-0021, PR #37 comment): fixed here: the scheduler compares with the live CR3, not a
  cached copy (exec, init and fork load CR3 themselves, and a freed PML4 comes back as the next child's);
  taking the BKL back after a sleep allows a held mutex; `sched_schedule` refuses a held spinlock when yielding
  too; trylocks skip the order check; `klog_console_acquire` waits at most 5 ms of TSC time and not at all when
  its own CPU owns the console; `locking.md` gives pipes their own BKL row. Left: `lockdep_release` ignores a
  lock missing from the CPU's list (locks taken before `sched_init` would false-fire); TTY output still takes
  the console once per character; the affinity read in `sched_select_cpu` is unlocked (M1-17 note); a woken
  thread on a busy CPU waits up to a slice (M1-06); an IRQ spinning on the BKL is deaf to IPIs (NMI note above).
- Tests: `qemu.sched` takes about 2 minutes under TCG (10 000 fork/exec rounds), so its timeout is 400 s.
  Exec no longer logs its segments at info level (each line redrew the framebuffer: ~50 ms per exec in TCG).
