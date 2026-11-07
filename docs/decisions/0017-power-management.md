# ADR-0017: Power management is a first-class requirement

- **Status:** Accepted (Michael, 2026-11-07)
- **Date:** 2026-11-07

## Context
With no grid, the machine may run on a laptop battery, a power bank or solar. Wasted power is lost uptime.
Power behaviour is cheap to design in now and expensive to retrofit: a kernel built around a periodic tick and
drivers with no power hooks would need rework.

## Decision
- **Tickless idle.** The scheduler and timer subsystem never require a periodic tick. When a CPU has nothing to run
  it programs the TSC-deadline timer for the next real event and sleeps.
- **Idle states.** The idle loop uses `MWAIT` with C-state hints when available (from ACPI `_CST` or CPU defaults),
  falling back to `HLT`.
- **CPU performance control.** Intel HWP when present: Energy Performance Preference (EPP) set from the active
  power profile. Fallback to ACPI `_PSS`/`_PPC` P-states is optional.
- **Power profiles:** `performance` (on AC), `balanced` (default), `saver` (on battery or below a threshold).
  The profile sets EPP, the inference thread count and core choice (P-cores vs E-cores), and display timeout.
  `arc-llm` reads the profile; the user can override it.
- **ACPI power devices via uACPI:** battery (`_BIF`/`_BIX`, `_BST`), AC adapter (`_PSR`), lid switch, power and sleep
  buttons. Exposed under `/sys/power` style files in procfs/devfs (exact paths fixed in M4) and as input events
  for buttons/lid.
- **Battery policy:** low-battery warning; at a critical level, sync filesystems and power off cleanly.
- **Display:** blank the framebuffer after an idle timeout; backlight brightness through ACPI video (`_BCM`/`_BQC`)
  where available.
- **Devices:** NVMe Autonomous Power State Transitions (APST) enabled; drivers stop polling when idle.
- **Measurement:** read Intel RAPL energy counters (package and core) so power use can be measured and tuned.
  A `power` command shows battery %, AC state, profile and watts.
- **Driver model hooks:** every driver has `suspend` and `resume` operations from the start (ADR-0006), even though
  system suspend is not implemented yet.

## Not in the first build
- **System suspend.** The high-tier laptop (Core Ultra 7 255H) most likely uses S0ix ("modern standby") rather
  than S3, which requires every device on the platform to reach low-power states and is a large project. The hooks
  above keep it open. Instead, the target is a **fast clean shutdown and fast boot**.
- PCIe ASPM tuning, USB autosuspend, GPU power gating beyond what firmware sets.

## Consequences
- Scheduler, timer and idle code are written tickless from M1; no code may assume a periodic interrupt.
- Rules out: busy-wait idle loops, drivers that poll forever, a fixed-HZ scheduler design.

## Verification
- M1: idle CPUs stay asleep (measured by RAPL and by counting timer interrupts over 60 s idle).
- M4: battery %, AC, lid and buttons work on the laptop; critical-battery shutdown tested by faking the threshold;
  idle power and power during inference recorded per profile on both test machines.
