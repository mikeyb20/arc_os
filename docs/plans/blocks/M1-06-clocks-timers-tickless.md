# M1-06 — Clocks, timers and tickless idle

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M1-05 | M1-17, M1-21 |

**Goal.** Accurate monotonic and wall-clock time, one-shot timers per CPU, and no timer interrupts on idle CPUs
(ADR-0017).

**Read first.**
- `docs/decisions/0017-power-management.md`
- `kernel/arch/x86_64/pit.c`, `lapic.c`, `kernel/proc/sched.c` (time slices), `kernel/drivers/acpi.c`
- Notes of M1-05

**Build.**
1. Clocksource: invariant TSC (CPUID 0x80000007 EDX bit 8). Frequency from CPUID 0x15/0x16, else calibrate against
   the PIT (or HPET if present) for about 50 ms. `ktime_get_ns()` is monotonic. Check TSC skew between CPUs at boot
   and log it.
2. Clock events per CPU: TSC-deadline (CPUID 1 ECX bit 24, MSR 0x6E0), else LAPIC one-shot calibrated to the TSC.
3. Per-CPU timer queue (min-heap): sleep timeouts, the scheduler slice, later futex and poll timeouts. Only the
   next deadline is programmed.
4. Tickless: the slice timer is armed only when a CPU has more than one runnable thread. An idle CPU has no timer
   unless a timeout is pending.
5. Wall clock: CMOS RTC read at boot (UTC) plus the monotonic offset. Syscalls: `clock_gettime` (`REALTIME`,
   `MONOTONIC`, `MONOTONIC_RAW`, `BOOTTIME`, `PROCESS_CPUTIME_ID`, `THREAD_CPUTIME_ID`), `clock_getres`,
   `nanosleep`, `clock_nanosleep` (relative and absolute), `gettimeofday`, `time`.
6. `/proc/interrupts` with per-CPU counts.
7. Tests: nanosleep within ±2 ms in TCG; monotonic time never goes backwards across CPUs; 5 s idle gives about
   zero timer interrupts on idle CPUs; RTC year ≥ 2025; preemption still works under load.

**Done when.**
- [x] Tests pass. The PIT is used only for calibration.
- [x] The `idle_timer_irqs_5s` budget (listed `off` by M0-08 in `tests/budgets.txt`) is measured and turned on.
- [x] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
  `arch/x86_64/`, and call the HAL through `hal_*` functions, not x86 names (work-blocks.md §7); the
  `arch_includes_outside_arch` limit in the budgets file is lowered to match (ADR-0020).

**Watch out.**
- TCG may report a TSC that isn't invariant. Fall back cleanly and log it.
- Put the LAPIC timer in TSC-deadline mode before writing the deadline MSR, and fence before the write (Intel SDM).
- Keep the old PIT path until the new one passes its tests, then delete it.

**Notes.**
- From M1-03: LAPIC registers go through `lapic_read`/`lapic_write` (xAPIC MMIO or x2APIC MSRs); TSC-deadline is an
  MSR in both modes. `smp_init` bounds its wait for APs with raw `rdtsc` (2^32 cycles) until a calibrated TSC exists;
  switch it to the clocksource here.
- From M1-04: `arch/x86_64/tsc.c` already calibrates the TSC at boot (`hal_tsc_calibrate`, `hal_tsc_info`,
  `hal_tsc_to_ns`), in this order: CPUID 0x15 (crystal × ratio), else PIT channel 2 for 50 ms
  (`pit_measure_tsc`, bounded by 2^32 cycles), else CPUID 0x16 base frequency. It also keeps the PIT figure when
  CPUID gives one, so the boot report and `arc.selftest` compare them; it records invariant TSC and
  TSC-deadline support. The spec's order puts 0x16 before the PIT: decide here (0x16 is the base clock, which
  is not always the TSC rate). `diag_delay_ms` (diag only) busy-waits on the TSC; replace it with the clocksource.
- From M1-05: the slice timer is the LAPIC timer in periodic mode, 100 ms (`SCHED_SLICE_NS`), per CPU:
  `lapic_timer_setup(sched_preempt)` measures its rate once against the TSC (`hal_slice_timer_init`), and the
  scheduler arms it on every switch to a non-idle thread and stops it on a switch to the idle thread
  (`hal_slice_timer_start/stop`, three APIC writes per switch). So idle CPUs already get no slice interrupts; the
  BSP's PIT still ticks at 100 Hz for `pit_get_uptime_ms` only (it no longer schedules). Replace both with the
  per-CPU timer queue; `sched_preempt` (skips a CPU holding a spinlock) stays the slice handler.
- From M1-10: the block layer's request timeouts (`blk_expire_all`, 30 s per request, O(1) when nothing is in
  flight) run from the PIT tick through `hal_timer_set_tick_hook` (kmain). When the PIT tick goes, arm a
  one-shot timer per disk at its oldest request's deadline instead (the head of `BlkQueue.fly_head`; re-arm on
  dispatch and completion), and drop the hook. Both `blkdev.c` and the diag code turn milliseconds into TSC
  cycles with `diag_ms_to_cycles`; switch them to the clocksource.
- **Built (M1-06).** Read these before touching time:
  - Layout: `arch/x86_64/tsc.c` (calibration, conversion, boot sync check), `hpet.c`, `rtc.c`, `lapic.c`
    (clock-event hardware); `kernel/time/clock.c` (clock IDs, wall clock, bring-up order), `timer.c` (per-CPU
    queues), `time_syscalls.c`. Portable code reaches the hardware only through the `hal_clock*` /
    `hal_clockevent_*` / `hal_rtc_read` calls in `arch/hal.h`.
  - Calibration order: CPUID 0x15, else the PIT (50 ms), else the HPET (20 ms, after ACPI, only if the PIT
    failed; `arc.tsccal=hpet` forces it in test mode), else CPUID 0x16, else a 1 GHz guess. 0x16 went last: it is
    the nominal base clock, which differs from the TSC rate on some parts. Conversions are mult/shift (32-bit
    shift, one 128-bit multiply, no divide).
  - The boot sync check runs every CPU at once (`ipi_call_all`, 20 ms, Linux's `check_tsc_warp` idea). Any step
    back turns on a clamp: every monotonic read goes through one shared atomic maximum (slower, never backwards).
  - Clock events: TSC-deadline when CPUID says so (LVT mode first, `mfence; lfence` before the MSR write), else
    LAPIC one-shot at divide 16, measured against the TSC once; `arc.timer=oneshot` forces one-shot (a fallback
    for deadline-timer errata on some Skylake/Kaby Lake microcode). One-shot may fire a little early; the queue
    just reprograms.
  - Timer queue: an rbtree with a cached leftmost per CPU, not the min-heap the spec named: no allocation and no
    capacity limit in interrupt context, O(log n) arm/cancel, O(1) next expiry (`lib/rbtree.h` already
    existed). A CPU programs only its own hardware; a CPU with an empty queue is never programmed.
  - Tickless scheduler: `rq_wants_slice` = a real thread runs and another is queued. A remote enqueue onto a busy
    CPU without a slice sends a resched IPI; that CPU arms its own slice (`sched_ipi`). Slice expiry only sets
    `need_resched`; `sched_timer_exit` switches after the timer queue is done; a CPU holding a spinlock retries in
    1 ms (`SCHED_RETRY_NS`). Wakeups still wait for the running thread's slice (up to 100 ms, as in M1-05): the
    load test measures a 1 s sleep waking ~104 ms late beside 8 CPU hogs.
  - PIT: silenced (`pit_stop`, IRQ0 masked in the IOAPIC); only `pit_measure_tsc` remains. The old tick path and
    `hal_timer_*` / `pit_get_uptime_ms` / `diag_ms_to_cycles` / `diag_delay_ms` are gone; use `clock_*`.
  - Block layer: one timer per disk at the oldest in-flight request's deadline, cancelled when nothing is in
    flight; `blk_poll_all` (boot-time page-cache waits) walks the disk list instead of a shared in-flight count.
  - Wall clock: RTC read once (UIP wait, two equal reads, BCD/binary, 12/24 h, FADT century register), taken as
    UTC. File times (`vfs_now`) are now wall-clock times. No settimeofday yet.
  - CPU time: `Thread.run_cycles`, added at each switch; `PROCESS_CPUTIME_ID` reads the main thread.
  - Measured (TCG, -smp 4): sleeps 0.2-1.3 ms late, idle CPUs 0 timer interrupts over 5 s, the sleeping CPU 1.
- **For later blocks.**
  - M1-16: `CLOCK_PROCESS_CPUTIME_ID` must sum the group's threads (`sched_thread_cpu_ns` per thread).
  - M1-17: no one wakes a sleeping thread for a signal yet. `timer_sleep_until` returns 1 on any other
    `sched_wake`, and nanosleep/clock_nanosleep already return `-EINTR` with the time left when
    `sig_work_pending()` is set, so signal delivery only has to wake the sleeper. `fpu_avx` forks 8 CPU-bound
    spinners so its workers share CPUs (a lone thread is never preempted now); pinning both workers to one CPU
    is the cleaner test once `sched_setaffinity` exists. Futex and poll timeouts: `timer_sleep_until` or a
    `Timer` under the waiter's lock (rules in `docs/kernel/locking.md`, "Timer locks").
  - M1-21: the LAPIC timer stops in deep C-states unless CPUID 6 EAX bit 2 (ARAT) is set. Before MWAIT into
    C3 or deeper on a CPU without ARAT, a pending timer needs another wake source (or stay at C1). Clock-event
    suspend/resume hooks also belong there.
  - Hardware: ADR-0021 smoke boot on both machines after merge; the report's new `Clock` line shows the timer
    mode, TSC sync and RTC result, and the self-test adds `tscsync` and `timer` (now 8 checks).
