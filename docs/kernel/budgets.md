# Budgets (ADR-0019)

CI checks sizes and counts the kernel reports against limits in `tests/budgets.txt`, so an efficiency regression
fails the PR that causes it. Never time: QEMU under TCG or a shared runner has no meaningful clock.

## How it runs

- The kernel prints `ARC_TEST budget <name> <value>` lines over serial in test mode (`ARC_BUDGET()` in
  `kernel/lib/budget.h`). Init prints the leak-check lines itself (`userland/init/init.c`).
- `tools/budget-check.py` reads the QEMU suites' serial logs (`build/qemu-test/<suite>.serial.log`) and
  `build/kernel.elf`, compares each value with its limit and prints one table.
- ctest runs it as `qemu.budgets` after the QEMU suites (fixture `qemu_logs`), so CI's `qemu` job fails on a breach,
  naming the metric. `qemu.budgets.selftest` checks that deliberately wrong limits fail by name.
- Run it alone after the suites: `python3 tools/budget-check.py`.
- Code hygiene counts (ADR-0020) come from `tools/hygiene.py`, which writes `<build-dir>/hygiene.log`. The host
  test `hygiene` runs it and checks only those metrics (`--only-source hygiene`), so CI's build job catches them
  without QEMU; `qemu.budgets` skips them.

## Rules

- `max` metrics fail more than 5% above the limit and warn more than 20% below it; `min` metrics are the mirror.
  A warning means the limit should come down in the PR that improved it.
- **Hygiene limits only go down.** They are exact ratchets: one more duplicate, copied helper, x86 include
  outside `kernel/arch/`, long function or untracked TODO fails (no 5% margin). A PR that removes one lowers the
  limit to the new count. Raising one needs Michael's OK and the reason in the PR.
- A metric nobody reports fails, so a removed `ARC_BUDGET` line can't silently drop a check.
- First limits are the measured value plus about 10% (minus for `min`), not guesses.

## Add a metric

1. In the kernel: `ARC_BUDGET("name", value);` where the value is known (boot metrics in `kmain` before init
   starts; end-of-run metrics in `report_end_budgets()` in `kmain.c`, which runs when init exits). User-space
   metrics: print the same line from init.
2. In `tests/budgets.txt`: `name limit max|min unit owner-block source`. `source` is `build` (measured by
   `budget-check.py` from the build; add the measurement there), `hygiene` (a count `tools/hygiene.py`
   prints), a suite name (that suite's log), or `*` (every
   suite that reports it; the worst value counts). Append `off` to list a metric that is not measured yet.
3. Run the suites, then `python3 tools/budget-check.py`, and set the limit from the measured value.

## Change a limit

Edit the line in `tests/budgets.txt` and say why in the PR: what got better (lower it) or what new cost is worth
it (raise it). Raising a limit needs the reason in the PR body (ADR-0019).

## Current metrics

| Metric | Source | Owner | What it measures |
|---|---|---|---|
| `kernel_image_bytes` | build | M0-08 | `.text + .rodata + .data + .bss` of `kernel.elf` |
| `boot_free_bytes` | smoke (`-m 4G`) | M0-08 | free memory when init starts (min) |
| `kernel_heap_bytes` | smoke | M0-08 | live kmalloc bytes when init starts, large (`alloc_pages`) blocks included |
| `page_descriptor_bytes` | smoke (`-m 4G`) | M1-02 | size of the `struct page` array (32 B per PFN up to the highest usable one) |
| `pages_leaked` | every suite | M0-10 | free pages lost between a suite's first and last test, minus kernel heap growth, large blocks included (limit 0) |
| `heap_leaked_bytes` | every suite | M0-10 | live kmalloc bytes gained over the same span (limit 0: a suite must remove files it creates) |
| `kmalloc_walk_max` | every suite | M1-02b | most heap blocks one kmalloc walked (first fit) |
| `idle_timer_irqs_5s` | off | M1-06 | timer interrupts on an idle CPU over 5 s |
| `dup_windows` | hygiene | M0-09 | duplicated code regions (10+ normalized lines found twice on one side) |
| `dup_static_names` | hygiene | M0-09 | extra copies of `static` functions with the same name on one side (N files add N − 1) |
| `arch_includes_outside_arch` | hygiene | M0-09 | `#include "arch/x86_64/..."` outside `kernel/arch/` |
| `long_functions` | hygiene | M0-09 | functions over 100 lines |
| `untracked_todos` | hygiene | M0-09 | `TODO`/`FIXME` without a block ID or `DEFERRED` |
