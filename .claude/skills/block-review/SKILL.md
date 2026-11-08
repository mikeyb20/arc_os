---
name: block-review
description: Pre-PR checklist every arc_os work block runs before opening its PR (scope, reuse, HAL boundary, work-blocks §7 rules, tests, hygiene counts, docs). Use at the end of any block, before `create_pull_request`.
---

# block-review

Run this after the block's code and tests are done and before the PR is opened (work-blocks.md §3 rule 13,
ADR-0020). It reads only the diff, the block spec and the files those name. Paste the result table into the PR body
under **Block review**. A FAIL inside the block's scope gets fixed before the PR; anything outside scope becomes a
line in the block's Notes.

## 0. Load
- `git fetch origin master` and `git diff origin/master...HEAD --stat`, then the diff of changed source files.
- The block spec `docs/plans/blocks/<ID>-*.md` (Build, Done when, Cost, Watch out, Notes).

## 1. Scope
- Every Build step and Done-when item is done, or explained in Notes.
- Nothing outside the spec. Ideas go in Notes, not code.
- If an interface a later spec names changed, that spec is updated in this PR (§3 rule 8).

## 2. Reuse (don't recreate code)
- For each new function, macro, constant table or struct: look it up in `docs/kernel/helpers.md`, then
  `git grep` for its name and for one distinctive line of its body across `kernel/`, `libc/`, `userland/`.
- An existing helper that does the job is used, not copied. A second copy of a kernel helper is promoted to the
  shared header in this PR and listed in `helpers.md`.
- Kernel vs user space is a real boundary: the same function in `kernel/lib` and `libc` is fine.

## 3. Boundaries
- No new `#include "arch/x86_64/..."` outside `kernel/arch/`; use `arch/hal.h` or arch-neutral headers.
  Arch includes in files this block rewrote are moved behind the HAL.
- Callers use a subsystem's public header, never its internals. Layers call downward only.
- New public functions have a doc comment in the header. New or rewritten `.c` files open with what they own,
  their invariants and the locks they take.

## 4. Kernel rules (work-blocks.md §7)
- User memory only through `copy_from_user`, `copy_to_user`, `get_user`, `put_user`, `strncpy_from_user`.
- Any lock an interrupt handler takes is IRQ-safe; lock order matches §7; no sleeping under a spinlock.
- No scan of a global list or array in allocate, free, fault, schedule, lookup or interrupt paths.
- Hot and ABI structs carry `_Static_assert`s; ABI structs live in `kernel/include/uapi/`.
- No periodic timers; drivers use the bus, IRQ and DMA helpers once M1-09 lands.
- The spec's **Cost.** section is true of the code as written.

## 5. Tests
- A host test for each behaviour change; the targeted QEMU test (`ctest -L qemu -R <name>`); the full QEMU suite
  once before the PR. Nothing skipped or loosened to pass.
- New metrics are in the budgets file (M0-08).

## 6. Hygiene counts
- `python3 tools/hygiene.py` (from M0-09): no count above master's. A count that fell gets its limit lowered here.
- Cleanup sits in its own `tidy:` commit, only in files the block already edits, about 10% of the block at most.
- No `TODO`/`FIXME` without a block ID or `DEFERRED.md` reference. Format check passes on changed lines.

## 7. Docs and handoff
- Reference files that restate code still match it (the syscall table passes its CI check).
- The block's Notes say what was done, decisions, surprises, next step (5–10 lines). Its row in work-blocks.md §4
  is updated.

## 8. Report
One table in the PR body: section, PASS / FAIL / N/A, evidence (`file:line` or the command run). State what was
verified and how. A clean block gets "no issues found", not padding.
