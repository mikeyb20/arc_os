# /review — Post-Build Diff Review

## Instructions

You are reviewing code you did NOT write. You have no context from the
implementation session. This is intentional — you catch things the building
agent is blind to because you don't share its assumptions.

Focus on correctness and robustness. Do NOT comment on style unless it
impacts readability or correctness.

## Step 1: Load Context

1. Identify the feature branch: `git branch --show-current`
2. Load the diff: `git diff main..HEAD`
3. Read the plan: check scratchpad(s) for the plan path, or find it in `docs/plans/`
4. Read CLAUDE.md for project conventions

## Step 2: Spec Compliance

For each item in the plan:
- Is it implemented? (yes / no / partial)
- Does the implementation match the specified approach?
- Are the interfaces exactly as defined in the plan?

Flag any deviations. Check scratchpad "Decisions Made" section —
documented deviations are acceptable if the rationale is sound.
Undocumented deviations are findings.

## Step 3: Convention Compliance (Delta-Scoped)

Only scan the CHANGED FILES — not the entire codebase.

| ID  | Check | Severity |
|-----|-------|----------|
| S01 | File naming: `snake_case` with `.c`/`.h`/`.asm` extension | ERROR |
| S02 | Header guards: `#ifndef ARCHOS_<PATH>_H` / `#define ARCHOS_<PATH>_H` | ERROR |
| S03 | Function names: `snake_case` | ERROR |
| S04 | Function prefix: public functions use `subsystem_` prefix | WARNING |
| S07 | Doc comments on every `.h` declaration | WARNING |
| S08 | 4-space indentation, no tabs | ERROR |
| S09 | K&R brace style | WARNING |
| A01 | Layer hierarchy violations (`#include` direction) | ERROR |
| A04 | Platform-specific code outside `arch/` | ERROR |
| A05 | Forbidden libc calls in kernel code (`printf`, `malloc`, `free`) | ERROR |
| C01 | Unchecked pointer dereference after fallible function | WARNING |
| C02 | Unchecked return values from functions that can fail | WARNING |
| C06 | Use-after-free patterns | ERROR |
| C10 | Pointer arithmetic on `void*` | ERROR |
| K03 | Large stack allocations (>512 bytes, VLAs) | WARNING |
| K05 | Hardcoded kernel addresses outside config headers | WARNING |

## Step 4: Integration Checks

| ID  | Check | What to Look For |
|-----|-------|------------------|
| I01 | Init order correctness | New init call respects layer hierarchy: lib < boot < arch < mm < drivers < proc < fs/ipc/net < security |
| I02 | No circular includes | Trace `#include` chains in changed headers |
| I03 | Consistent types | Types used across file boundaries match exactly |
| I04 | Spinlock usage for ISR-shared state | Global state accessed from ISR + normal context must be protected by spinlock with cli |
| I05 | EOI ordering | `pic_send_eoi()` called BEFORE handler logic (handler may context_switch away) |
| I06 | Thread safety for global state | New global/static mutable variables — check for multi-thread access |

## Step 5: Correctness Review

For each file in the diff:

### Error Handling
- Are all error paths handled?
- Any silent failures (errors caught but not logged or propagated)?

### Edge Cases
- Empty/null/zero inputs
- Boundary values (max int, empty collections, single element)
- Concurrent access (if applicable)
- Resource cleanup on error paths

### Safety (C kernel-specific)
- Undefined behavior, dangling references, buffer overflows
- Uninitialized memory, missing volatile on MMIO
- Unchecked pointer dereference after fallible function

### Logic
- Do conditionals cover all cases?
- Are loop bounds correct?
- Are off-by-one errors present?

## Step 6: Report

```markdown
## Review: <feature name>

### Spec Compliance
<pass/fail per plan item, with notes on documented deviations>

### Convention Violations (<count>)
- [ID] file:line: description

### Integration Issues (<count>)
- [ID] file:line: description

### Issues Found

#### Critical (must fix before merge)
<numbered list with file:line, description, and fix recommendation>

#### Important (should fix)
<numbered list>

#### Minor (optional improvements)
<numbered list>

### Verdict: <APPROVE / REVISE>
```

If REVISE: list specific items that must be addressed. After fixes are made,
a new `/validate/review` session should be run before proceeding to `/validate/done-check`.
