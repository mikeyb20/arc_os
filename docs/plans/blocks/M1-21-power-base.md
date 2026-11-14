# M1-21 — Power base: idle states, HWP and energy readout

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | K | M1-06 | M1-22, M4-12 |

**Goal.** Idle CPUs sleep as deeply as is safe, the CPU's own frequency control (HWP) runs with an energy
preference set by the power profile, and package energy can be read (ADR-0017).

**Read first.**
- `docs/decisions/0017-power-management.md`
- `kernel/arch/x86_64/msr.h`, `percpu.h`, the idle loop in `kernel/proc/sched.c`, Notes of M1-06

**Build.**
1. `rdmsr_safe`/`wrmsr_safe` with exception-table fixups (#GP returns an error instead of crashing).
2. Idle: MWAIT when CPUID 1 ECX bit 3 and leaf 5 report it. Start with C1 (hint 0x00) plus the deepest state from
   a small per-model table for the two test CPUs (7th/8th-gen Core i3, Core Ultra 200H); HLT otherwise. Count
   residency per state.
3. HWP: when CPUID 6 EAX bit 7 is set, enable it (`IA32_PM_ENABLE` 0x770) and program `IA32_HWP_REQUEST` (0x774)
   on every CPU: min/max from `IA32_HWP_CAPABILITIES` (0x771), EPP (bits 31:24, when EAX bit 10 is set) from the
   profile. Without HWP, leave the firmware's settings and log it.
4. Profiles `performance`, `balanced` (default), `saver` (ADR-0017). In this block a profile sets EPP only; M4-12
   adds thread count, core choice and display timeout.
5. RAPL: `MSR_RAPL_POWER_UNIT` (0x606), package (0x611), cores (0x639), platform/PSYS (0x64D) when present; handle
   32-bit wraparound by sampling on every read.
6. Provisional readout and control at `/proc/arc/power` (profile, HWP and EPP state, energy in joules, average watts
   since the last read). ADR-0017 fixes the final paths in M4; keep the kernel side behind one interface so only
   the file names change.
7. Host tests for the unit and EPP maths; QEMU: no MWAIT → HLT path works; reading a missing MSR returns an error.

**Done when.**
- [ ] Tests pass. Real readings are taken in M1-22.

**Watch out.**
- MWAIT leaf-5 data doesn't say which states are safe on a given board; stay conservative until measured.
- On hybrid parts HWP requests are per logical CPU.

**Notes.**
- From M1-05: the idle loop is `sched_idle_loop` (proc/sched.c): it halts through `hal_idle_wait` (`sti; hlt`)
  with interrupts off before the check, so a wakeup IPI is never missed. MWAIT and C-state choice go behind that
  call. The idle thread's time is counted per CPU (`/proc/sched_debug` `idle_ns`).
- From M1-06: idle is `sti; hlt` with each CPU's clock event programmed only for its earliest timer (none
  when its queue is empty). The LAPIC timer stops in deep C-states unless CPUID 6 EAX bit 2 (ARAT) is set:
  before MWAIT into C3 or deeper on a CPU without ARAT, a pending timer needs another wake source, or the CPU
  stays at C1. The clock-event and TSC code has no suspend/resume hooks yet (`lapic_timer_cpu_init` reprograms
  the LVT; the queue reprograms on its next arm); add them here. `arc.timer=oneshot` forces LAPIC one-shot.
