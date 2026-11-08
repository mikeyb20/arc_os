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
- [x] CI runs the budget check after the QEMU suites; a breached limit fails the job with its name.

**Cost.** Adds seconds to the existing QEMU run; no extra boot.

**Watch out.**
- Set first limits from the measured values plus about 10%, not from guesses.
- Never add a wall-clock metric.

**Notes.**
- Done 2026-11-08. Files: `tests/budgets.txt` (limits; format `name limit max|min unit owner source [off]`),
  `kernel/lib/budget.h` (`ARC_BUDGET`, prints only in test mode), `tools/budget-check.py` (checker),
  `docs/kernel/budgets.md` (how to add a metric or change a limit), ctest `qemu.budgets` and
  `qemu.budgets.selftest` (fixture `qemu_logs`, so they run after the suites; CI's `qemu` job runs them with
  `ctest -L qemu`, no workflow change).
- Added a `source` column the spec didn't name: `build`, a suite name, or `*` (worst over every suite). Boot
  metrics come from `qemu.smoke`, which now boots with `-m 4G` as step 3 asks; other suites keep 256M.
- Leak check deviation: free pages at "boot" vs "after the suites" can't be equal, because init and its address
  space are still alive at the end. Init instead reads `/proc/meminfo` before its first test and after its last
  (every child reaped) and prints `pages_leaked` (MemFree drop minus kernel heap growth) and `heap_leaked_bytes`
  (HeapUsed growth). End-of-run kernel metrics use a new `testmode_set_end_hook()` (runs when init exits).
- Surprise: the leak check found that every exited process leaks ~54 pages and ~40 KB of kmalloc: `proc_reap()`
  only marks the child terminated (address space, page tables, kernel stack, fd table and `Process` are never
  freed, and `proc_list` only grows). Michael approved a new block, M0-10 (spec written), after M0-09; until
  then both leak limits are frozen at the measured worst (userland suite).
- Measured 2026-11-08 (TCG, deterministic across runs): kernel image 181251 B, boot free 4251873280 B at 4G,
  heap at init start 2082480 B, kmalloc walk worst 215 blocks, leaks 522 pages / 397216 B (userland). Limits =
  these ±~10%. CI uses KVM; if KVM counts differ, the first CI run shows it.
- `kmalloc_walk_max` costs one compare per kmalloc under the existing heap lock.
