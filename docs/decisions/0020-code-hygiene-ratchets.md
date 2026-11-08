# ADR-0020: Code hygiene ratchets and block review

- **Status:** Accepted (Michael, 2026-11-08)
- **Date:** 2026-11-08
- **Extends:** ADR-0019 (its budgets file and CI check now also hold code-structure counts). Supersedes nothing.

## Context
The repo's written rules (CLAUDE.md, work-blocks.md §7, ADR-0019) are good, but only efficiency has a CI check.
A look at master (4f0cc22, about 20k lines of C) found drift that nothing catches:
- **Duplicate helpers.** `phys + vmm_get_hhdm_offset()` is open-coded in at least 6 files (drivers, `mm`,
  `proc/elf.c`, `smp.c`); there are two private `phys_to_virt` (`kernel/mm/vmm.c`, `kernel/drivers/acpi.c`) and
  two private `hhdm_offset` copies (`kernel/mm/pmm.c`, `kernel/mm/vmm.c`). `mode_to_rwx` is copied in
  `userland/coreutils/ls.c` and `userland/shell/shell.c`.
- **HAL boundary.** The plan says RISC-V comes later "through the HAL", but 44 `#include "arch/x86_64/..."` lines
  sit in 8 subsystems outside `kernel/arch/`, and `kernel/arch/hal.h` is used by 2 files. Left alone, a port means
  editing every subsystem.
- **Style** is checked by eye; there is no `.clang-format`.
- **Review** differs per session; `/validate/review` diffs against `main` (the branch is `master`) and reads
  scratchpads, which work-blocks.md §3 rule 7 retired.
- **Docs that restate code** (the arc-os-architecture skill's `reference/` files) go stale silently.

A one-time cleanup would compete with plan work and would be needed again later. The cheaper path is to stop
growth now and pay down old cases only in code a block is already rewriting.

## Decision
1. **Ratchets.** `tools/hygiene.py` (Python 3 standard library only) counts structural patterns and reports them as
   `max` metrics in the ADR-0019 budgets file. A limit starts at the measured count and may only go down; raising
   one is a reviewed change with a reason, as in ADR-0019. Counts, never time. First metrics, over `kernel/`,
   `libc/` and `userland/` (vendored files such as `limine.h` excluded):
   - duplicated code windows (10 or more normalized lines appearing in two places);
   - the same `static` function name defined in two files (a likely copied helper; known legitimate pairs, such as
     kernel `kernel/lib` vs user `libc` string functions, are listed in the script with a reason);
   - `arch/` includes outside `kernel/arch/`;
   - functions longer than 100 lines;
   - `TODO`/`FIXME` without a block ID (`M1-07`) or `DEFERRED.md` reference.
2. **Formatting.** A `.clang-format` encodes the CLAUDE.md style. CI checks changed lines only
   (`git clang-format --diff` against the merge base), so the tree is never mass-reformatted.
3. **One home per helper.** `docs/kernel/helpers.md` names the canonical helper for each common job. Search before
   writing a `static` helper; a second copy in kernel code is promoted to the shared header in the same PR.
4. **HAL boundary.** New code outside `kernel/arch/` includes `arch/hal.h` or arch-neutral headers, never
   `arch/x86_64/` directly. Old includes are moved by the blocks that already rewrite those call sites
   (M1-03, M1-05, M1-06, M1-07, M1-09); each of those specs says so.
5. **Interface first.** A block that adds or changes a subsystem writes the header, its doc comments and a host test
   before the implementation. Each new or rewritten `.c` file opens with a short comment: what it owns, its
   invariants, the locks it takes.
6. **Tidy where you already are.** A block may remove ratchet entries in files it already edits, up to about 10% of
   its size, in a separate `tidy:` commit. No standalone refactor PRs. Each milestone's final block reads the
   hygiene report and tightens limits. Whatever pre-plan code is still left after M5 is removed in one legacy sweep,
   which M5-00 plans as a block.
7. **Block review.** Every block runs `.claude/skills/block-review` before its PR and puts the result in the PR
   body (work-blocks.md §3 rule 13).
8. **Generate, check or delete docs that restate code.** Reference files that only copy facts from code shrink to
   pointers at the headers. Files that also carry planning data, such as the syscall table's status and "needed by"
   columns, stay hand-written and get a CI consistency check against the code. Specs and ADRs stay hand-written:
   they record decisions.

## Consequences
- Adds block M0-09 (S): +0.5 session units. Ongoing cost is a few minutes of checklist per block.
- Old drift is frozen, not fixed: the counts fall as M1 rewrites the code.
- Rules out: a second copy of a kernel helper, new direct `arch/x86_64/` includes outside arch, unbounded functions,
  untracked TODOs, mass reformatting, standalone refactor PRs.
- Left out on purpose: GCC `-fanalyzer` (available in the pinned GCC 14.1, but slow and noisy on kernel code;
  revisit at M1-22) and per-subsystem owners (Michael owns everything).

## Verification
- M0-09: CI fails, naming the metric, when a deliberately duplicated function or a new `arch/x86_64/` include is
  added on a test branch; the first limits equal the measured counts on master.
- Each M1 block that touches the HAL lowers the `arch_includes_outside_arch` limit.
