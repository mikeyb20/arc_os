# M0-09 — Code hygiene checks

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| S | R | I | M0-08 | — |

**Goal.** CI fails when a PR adds duplicated code, a copied helper, a direct x86 include outside `kernel/arch/`, an
oversized function or an untracked TODO, measured against limits frozen at today's counts (ADR-0020).

**Read first.**
- `docs/decisions/0020-code-hygiene-ratchets.md`
- `docs/kernel/budgets.md` and the budgets file from M0-08 (paths in M0-08's Notes), `.github/workflows/ci.yml`

**Build.**
1. `tools/hygiene.py`, Python 3 standard library only, scanning `kernel/`, `libc/` and `userland/` (skip
   `kernel/include/limine.h` and other vendored files). It prints one `ARC_TEST budget <name> <value>` line per
   metric, the same format the M0-08 checker reads, plus a short list of the offending locations:
   - `dup_windows`: windows of 10 or more consecutive lines that appear twice, after stripping comments, blank lines
     and whitespace;
   - `dup_static_names`: `static` function names defined in more than one file, minus an allow-list in the script
     where each entry gives its reason (kernel `kernel/lib` vs user `libc` string and memory functions;
     `kmalloc.c` vs `libc/src/malloc.c` until M1-02b);
   - `arch_includes_outside_arch`: `#include "arch/x86_64/..."` (any relative form) in files outside `kernel/arch/`;
   - `long_functions`: functions over 100 lines;
   - `untracked_todos`: `TODO`/`FIXME` without a block ID (`M1-07`) or `DEFERRED` reference.
2. Add the five metrics to the budgets file as `max`, owner M0-09, with the limit equal to the measured count (not
   +10%: these only go down). The checker's "warn when 20% better" prompts tightening.
3. `.clang-format` matching CLAUDE.md (K&R braces, 4 spaces, no tabs). A CI step runs `git clang-format --diff`
   against the PR's merge base and fails on changed lines only. Install `clang-format` in `tools/dev-setup.sh`.
4. CI: run `tools/hygiene.py` in the build job (no QEMU needed) and feed its lines to the M0-08 checker
   (`tools/budget-check.py`). Its budgets-file lines need a source: write the output to a log the checker reads
   (e.g. `build/hygiene.log`) and add a `hygiene` source next to `build`, suite name and `*`
   (`docs/kernel/budgets.md`). The checker fails on a metric nobody reports, so the build job runs the checker
   on the hygiene lines only, or the QEMU-sourced metrics are skipped there.
5. `docs/kernel/helpers.md`: one table, job → canonical helper → header (phys↔virt, user copy, locks, lists,
   string/mem in `kernel/lib`, libarc on the user side). Name the current duplicates as known debt with the block
   expected to remove each, as a `tidy:` commit (HHDM helpers: the first block that edits `kernel/mm/pmm.c` or
   `vmm.c`, likely M1-02; `mode_to_rwx`: the next block that edits `ls` or the shell).
6. Docs that restate code (ADR-0020 decision 8). The syscall table
   (`.claude/skills/arc-os-architecture/reference/syscall-table.md`) also tracks status and who needs each call, so
   it stays hand-written but checked: `tools/hygiene.py` fails when the syscalls that have a handler differ from the
   rows marked done or partial. Reduce `hal-interface.md` and `memory-layout.md` in the same folder to pointers at
   the headers they describe wherever they only restate them.
7. Tests: a host test runs `tools/hygiene.py` on a tiny fixture tree with one known instance of each pattern and
   checks every count.

**Done when.**
- [x] CI fails, naming the metric, on a test branch that adds a duplicated function and an `arch/x86_64/` include.
- [x] The first limits equal the counts measured on master; `docs/kernel/budgets.md` says these limits only go down.

**Cost.** One pass over about 20k lines in CI, seconds; no extra QEMU boot.

**Watch out.**
- Keep the duplicate detector simple (hash of normalized line windows). A clever detector costs more to maintain
  than the duplicates it finds.
- Don't fix existing duplicates or includes in this block. They are frozen and fall as M1 rewrites the code.
- The format check must never touch unchanged lines.

**Notes.**
- Done 2026-11-08. `tools/hygiene.py` (0.4 s on the tree) prints the five counts plus every location; host test
  `hygiene` writes `build/hygiene.log` and runs `tools/budget-check.py --only-source hygiene`, so CI's build job
  fails on a rise without QEMU (`qemu.budgets` now uses `--skip-source hygiene`). `tools_hygiene` is the fixture
  test (one instance of each pattern, temp tree) plus checker tests.
- Measured on master cb25815: dup_windows 12, dup_static_names 2 (extra copies: `phys_to_virt`, `mode_to_rwx`),
  arch_includes_outside_arch 44 (matches ADR-0020), long_functions 3 (`kmain`, `vfs_setup`,
  `socket_tcp_deliver`), untracked_todos 0. Limits = these counts.
- Decisions: hygiene counts are exact ratchets in the checker (source `hygiene` gets no 5% margin; with 5%, 44
  would allow 46). Duplicates are counted as regions (merged runs of duplicate 10-line windows), per side
  (kernel vs libc+userland); `#include` lines and lone braces are ignored so shared include lists don't count.
  The `static` allow-list is empty: kernel/libc pairs are on different sides already.
- `.clang-format`: LLVM base, 4 spaces, 120 columns, K&R, includes not sorted, comments not reflowed. CI runs
  `git clang-format --diff` against the base branch tip on pull requests only (kernel, libc, userland, tests).
  Formatting the whole tree would change ~2400 of 21400 lines (aligned `#define`s and assignments, `asm volatile (`
  spacing), which is why only changed lines are checked. `clang-format` added to `tools/dev-setup.sh`.
- Syscall-table check: the 47 `syscall_register` handlers match the done/partial rows exactly.
- `hal-interface.md` and `memory-layout.md` were stale (claimed no `hal.h`, a fixed HHDM offset); now pointer
  tables. `docs/kernel/helpers.md` lists canonical helpers and the known duplicates with the block removing each.
- Done-when check: PR #12's commit bb3ba97 copied `mode_to_rwx` into `stat_cmd.c` and added an `arch/x86_64/io.h`
  include to `vfs.c`; CI run 42488853310 failed in the build job's `hygiene` test with `dup_windows = 13`,
  `dup_static_names = 3` and `arch_includes_outside_arch = 45`, each by name. The next commit reverted it.
