# guide-implementation

Active guardrail that tracks progress against an implementation plan and enforces arc_os conventions at each step. Detects scope creep, blocks completion without tests, and ensures the correct implementation order. This is the second step in the plan → implement → verify lifecycle.

## When to Use

Invoke after `/plan-feature` has produced a plan, or when starting implementation of a feature you want to track step-by-step. Use during coding sessions to stay on track, catch convention violations early, and ensure nothing is forgotten.

## Scope Determination

- **With prior plan**: `/guide-implementation` → consume the plan from the current conversation context (from a prior `/plan-feature` invocation)
- **With description**: `/guide-implementation add PCI enumeration` → infer scope from the description and current git changes
- **Resume**: `/guide-implementation` in a session with existing implementation in progress → detect current step from file state

---

## Execution Flow

### Step 1: Establish Plan

Determine the implementation plan to track against:

1. **From context**: If `/plan-feature` was invoked earlier in this conversation, use that plan directly. Extract the file list, API design, init placement, test plan, and implementation order.

2. **From description**: If no prior plan exists, infer scope:
   - Read files mentioned by the user or recently modified (`git diff --name-only`)
   - Read `kernel/boot/kmain.c`, `kernel/CMakeLists.txt`, `tests/test_main.c`, `tests/CMakeLists.txt` to understand current state
   - Build a minimal plan: files to create/modify, init placement, test expectations

3. **From git state**: If resuming, detect progress:
   - `git diff --name-only` to find changed files
   - Categorize each as header/impl/asm/build/init/test
   - Determine which steps are already complete

Print the plan summary and current progress before proceeding.

### Step 2: Track Implementation Steps

Monitor progress through 8 ordered steps. Each step has a **gate** — a set of conditions that must be met before it is considered complete. Report status as a table after each significant action.

| Step | Name | Gate Conditions |
|------|------|-----------------|
| 1 | **Header** | File exists at correct path. Guard format: `#ifndef ARCHOS_<PATH>_H` / `#define ARCHOS_<PATH>_H`. Every public function has a doc comment. Function names use subsystem prefix. No implementation code (function bodies) in the header — only `static inline` permitted. Types use `PascalCase` or `snake_case_t`. |
| 2 | **Minimal impl** | File exists at correct path. `#include` of own header is the first project include. Contains init function with `kprintf("[<NAME>] ...")` confirmation message. File compiles (no syntax errors). Other functions may be stubs returning error/NULL. |
| 3 | **Build system** | `.c` file listed in `KERNEL_C_SOURCES` in `kernel/CMakeLists.txt`. `.asm` file (if any) listed in `KERNEL_ASM_SOURCES`. Build succeeds: `cmake --build build` returns 0. |
| 4 | **Init call** | `#include "<layer>/<name>.h"` added to `kernel/boot/kmain.c`. `<name>_init(...)` call placed at correct position in init sequence (after dependencies, before consumers). Comment above the init call describes what it initializes. |
| 5 | **Core impl** | All functions declared in the header are implemented (no remaining stubs). Functions match header declarations exactly (parameter types, return types). Error paths handle failures (NULL checks after allocations, error return codes). K&R brace style. 4-space indentation. No tabs. Functions under 80 lines. |
| 6 | **Tests** | File `tests/test_<name>.c` exists. Uses `#include "../kernel/<layer>/<name>.c"` pattern. Header guards defined for conflicting kernel headers. Types reproduced from guarded headers. All external dependencies stubbed as `static` functions. Failure injection via `kmalloc_force_fail` or equivalent. `reset_<name>_state()` function cleans up between tests. 8+ test cases minimum. `TestCase <name>_tests[]` array exported. `int <name>_test_count` exported. Suite registered in `tests/test_main.c` (extern declarations + Suite array entry). Test file added to `tests/CMakeLists.txt` `add_executable` list. CTest entry added: `add_test(NAME test_<name> COMMAND test_runner --suite <name>)`. Tests pass: `ctest --test-dir build_host` returns 0 for the suite. |
| 7 | **Boot verify** | Cross-compile build succeeds. QEMU boot test passes (no PANIC, TRIPLE FAULT, or hang). Init confirmation message appears in serial output: `[<NAME>]`. |
| 8 | **Assembly** | (If applicable) File uses NASM syntax. Under 100 lines. Listed in `KERNEL_ASM_SOURCES`. Preserves callee-saved registers (RBX, RBP, R12-R15). Uses correct argument registers (RDI, RSI, RDX, RCX, R8, R9). Transitions to C as quickly as possible. |

### Step 3: Enforce Implementation Order

**Required order**: Header (1) → Minimal impl (2) → Build system (3) → Init call (4) → Core impl (5) → Tests (6) → Boot verify (7) → Assembly (8, if needed)

If the agent or user attempts to work out of order, redirect:

| Violation | Response |
|-----------|----------|
| Writing `.c` before `.h` | "Header first — create `<name>.h` with the API declarations before implementing." |
| Writing tests before impl | "Implementation first — at minimum, the header and a compiling stub are needed before tests can include the `.c` file." |
| Skipping build system | "Add to build system now — the `.c` file needs to be in `KERNEL_C_SOURCES` before we can verify it compiles in the kernel build." |
| Skipping init call | "Add the init call to `kmain.c` now — we need to verify init ordering before writing more code." |
| Skipping tests entirely | "Tests are required — create `tests/test_<name>.c` with 8+ tests before marking this feature complete." |
| Skipping debug output | "Add `kprintf` init confirmation — every subsystem must announce itself during boot." |

### Step 4: Scope Creep Detection

After each file modification, check for scope creep:

1. **Unplanned files**: Any file created or modified that is not in the plan's file list. Flag and offer three options:
   - **Add to plan**: Update the plan to include this file (justified change)
   - **Remove**: Revert the change (accidental scope creep)
   - **Defer**: Add a TODO comment and track as deferred work

2. **Unplanned functions**: New public functions not in the API design. Flag and offer the same three options.

3. **Unplanned includes**: New `#include` directives that pull in headers not in the dependency map. Check for layer violations (upward dependencies). Flag violations as errors.

4. **Unplanned features**: If the implementation adds capabilities beyond the plan (extra parameters, additional modes, new error codes), flag as potential scope creep.

Present scope creep findings as:

```
### Scope Creep Alert

| Item | Type | Planned? | Action Needed |
|------|------|----------|---------------|
| `kernel/drivers/pci_legacy.c` | New file | No | Add / Remove / Defer |
| `pci_scan_legacy_bus()` | New function | No | Add / Remove / Defer |
| `#include "drivers/acpi.h"` | New include | No (upward dep!) | Remove — layer violation |
```

### Step 5: Progress Report

After each significant action (file created, file modified, build attempted, test run), update and display the progress table:

```
## Implementation Progress: <Feature Name>

| Step | Status | Notes |
|------|--------|-------|
| 1. Header | DONE | `kernel/drivers/pci.h` — 6 functions, guard OK, docs OK |
| 2. Minimal impl | DONE | `kernel/drivers/pci.c` — compiles, init prints "[PCI]" |
| 3. Build system | DONE | Added to KERNEL_C_SOURCES |
| 4. Init call | DONE | kmain.c line 112, after kmalloc_init() |
| 5. Core impl | IN PROGRESS | 4/6 functions complete |
| 6. Tests | PENDING | — |
| 7. Boot verify | PENDING | — |
| 8. Assembly | N/A | No assembly needed |

**Convention issues**: None
**Scope creep**: None detected
**Next action**: Implement `pci_read_config()` and `pci_write_config()`
```

---

## Behavioral Rules

These rules are non-negotiable and override any user request to skip:

1. **Never skip tests** — Block completion (Step 7) if Step 6 is not DONE. A feature without tests is not complete.
2. **Never skip debug output** — Every init function must call `kprintf`. Redirect if missing.
3. **Header before implementation** — Redirect if `.c` is written before `.h` exists.
4. **Build after each file change** — Verify the build still succeeds after adding/modifying any kernel file.
5. **One subsystem at a time** — Do not start a second subsystem's header while the first is still in progress.
6. **Static stubs in tests** — All test stubs must be `static` to avoid linker clashes across test files.
7. **No kernel/include in tests** — Test files must NOT include from `kernel/include/` (freestanding headers conflict with system headers).

---

## Edge Cases

- **No prior plan exists**: Build a minimal plan from the user's description and current git state. The plan may be incomplete — track what is known and flag unknowns.
- **Plan changes mid-implementation**: Update the plan. Re-evaluate init ordering, dependencies, and test requirements. Report what changed.
- **User requests skipping a step**: Explain why the step is required. If the user insists on skipping tests, warn that `/verify-changes` will flag the gap, but do not block the user's explicit override.
- **Feature has no assembly**: Mark Step 8 as N/A.
- **Feature has no init function**: Mark Step 4 as N/A (e.g., utility libraries). Still require build system and test integration.
- **Extending an existing subsystem**: Steps 1-2 may be partially complete (header and impl already exist). Start from the first incomplete step.
- **Build fails during implementation**: Stop and fix the build error before continuing. Do not proceed to the next step with a broken build.
- **Tests fail**: Fix the failing tests or the implementation bug before marking Step 6 as DONE.

## Integration with Lifecycle

- **Input from**: `/plan-feature` — consumes the implementation plan
- **Output feeds into**: `/verify-changes` — after all steps are DONE, run verify-changes to audit the delta
- **Convention checks aligned with**: `/audit-code` — the gate conditions in Step 2 enforce the same conventions (header guards, function prefixes, K&R style, doc comments)
