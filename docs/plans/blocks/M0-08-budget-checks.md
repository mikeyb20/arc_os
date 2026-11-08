# M0-08 — Budget checks in the QEMU harness

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| S | R | I | M0-02 | — |

**Goal.** CI fails when a size or count the kernel reports passes its checked-in limit, so efficiency regressions
show up in the PR that causes them (ADR-0019). Counts and sizes only: TCG timing is never a budget.

**Read first.**
- `docs/decisions/0019-efficiency-budgets-and-allocators.md`
- The M0-02 harness files named in its Notes, `.github/workflows/ci.yml`

**Build.**
1. `tests/budgets.txt`: one line per metric with name, limit, `max` or `min`, unit and the block that owns it.
2. `ARC_BUDGET(name, value)` in one kernel header prints `ARC_TEST budget <name> <value>` over serial, so any block
   adds a metric with one line.
3. First metrics: kernel image size (`.text + .data + .bss`, measured at build time on the host); free memory after
   boot with `-m 4G` (min); kernel heap bytes after boot; free pages after the QEMU suites equal to the boot
   baseline (leak check); kmalloc blocks walked per call, worst case (replaced by slab steps in M1-02b). Timer
   interrupts on an idle CPU over 5 s is listed but disabled until M1-06.
4. The harness compares values with limits: fail when more than 5% past a limit, warn when 20% better (so limits
   get tightened), and print one summary table.
5. `docs/kernel/budgets.md`: how to add a metric and how to change a limit (the PR says why).
6. Tests: a test-only limit set deliberately low makes the check fail with the metric named.

**Done when.**
- [ ] CI runs the budget check after the QEMU suites; a breached limit fails the job with its name.

**Cost.** Adds seconds to the existing QEMU run; no extra boot.

**Watch out.**
- Set first limits from the measured values plus about 10%, not from guesses.
- Never add a wall-clock metric.

**Notes.**
-
