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
- **Built (M1-21).** Read these before touching idle, HWP or energy:
  - Layout: `arch/x86_64/power.c` (hardware: idle-state table, MWAIT/HLT entry with residency, HWP, RAPL),
    `msr_safe.asm` (`rdmsr_safe` / `wrmsr_safe`), `kernel/power/power.c` (profiles, the idle loop's sleep length,
    the status snapshot). Portable code reaches the hardware only through `hal_power_*`, `hal_idle_enter`,
    `hal_idle_stats`. `/proc/arc/power` (procfs.c) is the only user-facing file; M4 renames it.
  - Safe MSRs: two exception-table entries; kernel-mode #GP now looks the RIP up in `.ex_table` (isr.c), so the
    user copy also fails with `-EFAULT` instead of panicking if it is ever handed a non-canonical address. QEMU's
    TCG reads and writes unknown MSRs without #GP (all read 0), so probes also treat a 0 unit register or a 0
    counter as absent; only KVM (CI, `qemu.power.msr`) and real CPUs show the fixup.
  - Idle: MWAIT only on Intel, only when leaf 5 has both the extensions and interrupt-break bits and lists a C1
    sub-state (Linux's rule; TCG's EDX is 0, so QEMU idles on HLT). States: C1 (0x00) plus one deep state for
    family 6 models 0x8E/0x9E (Kaby/Coffee Lake: C6 0x20, exit 85 us, residency 200 us, Linux `skl_cstates`) and
    0xC5 (Arrow Lake H: C6 0x20, 140/420 us; Linux has no ARL table, these are its Meteor Lake L numbers). The
    hint must be listed in leaf 5 EDX; without ARAT only C1. Governor: deepest state whose residency fits the
    time to this CPU's programmed timer (`timer_next_event`; none = deepest). MWAIT runs with interrupts masked
    and ECX bit 0 (an interrupt ends it), so the residency is closed before the handler runs; HLT's handler runs
    first and may switch threads, so the interrupt entry closes it (`power_idle_close` in `isr_dispatch`).
    Residency is per CPU behind a seqcount and includes the sleep in progress (a tickless CPU may sleep for
    seconds). `arc.idle=hlt|c1` caps it for bring-up.
  - HWP: each CPU enables it and builds its own request (min = lowest, max = highest from its own capabilities,
    desired 0, activity window and package bit kept, EPP when supported) the first time it syncs. A profile change
    bumps a generation and broadcasts a reschedule IPI; each CPU applies it in that IPI or before its next sleep
    (no cross-CPU call that waits). EPP: performance 0, balanced 128, saver 192 (Linux's strings). HWP stays on
    once enabled (until reset).
  - RAPL: units from 0x606; package, cores, platform present when they read non-zero at boot; 32-bit counters
    extended under `rapl_lock` at every read and at most once a second from an idle CPU (trylock). A wrap is lost
    only if every CPU stays busy and nothing reads for a whole wrap (about 40 min at 100 W with 61 uJ units).
    PSYS uses the same energy unit as the package (Linux does too on client parts; check in M1-22).
  - Costs: an idle entry adds one clock read, two TSC reads and two compares; the interrupt entry one per-CPU
    load. Image +14950 bytes (tests/budgets.txt).
  - Measured (TCG, -smp 4): `qemu.power` idle residency +4.0 s over a 1 s sleep on 4 CPUs, 2 idle entries.
- **For later blocks.**
  - M1-22: read `/proc/arc/power` and the report's `Power` line on both machines (idle method and C6 accepted, HWP
    CPUs, RAPL domains); record idle watts per profile. If C6 misbehaves, `arc.idle=c1` is the fallback. The
    deep-state values are Linux's, not measured.
  - M4-12: profiles add thread count, core choice and display timeout; the final file names replace
    `/proc/arc/power` (only procfs.c changes). Battery/AC switching of profiles belongs there too.
  - Not taken here (not in this spec): clock-event suspend/resume hooks (M1-06 note; no system suspend exists to
    call them), the BKL on time syscalls, `qemu.fpu` tick counting, the 100 ms wake latency under load.
  - Other low-tier CPUs (6th-10th gen: Skylake 0x4E/0x5E, Comet Lake 0xA5/0xA6) get MWAIT C1 only until someone
    adds a measured row to `idle_models`.

