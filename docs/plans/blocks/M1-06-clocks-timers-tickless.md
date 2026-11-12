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
- [ ] Tests pass. The PIT is used only for calibration.
- [ ] The `idle_timer_irqs_5s` budget (listed `off` by M0-08 in `tests/budgets.txt`) is measured and turned on.
- [ ] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
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
