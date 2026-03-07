# refactor

Scan kernel source files for complexity, duplication, and readability issues. Produce a prioritized plan of 5–10 refactors with exact before/after code. Apply each refactor one at a time, running build + boot tests between each. **Never adds functionality** — only simplifies.

## When to Use

Invoke when code works but is hard to read, overly complex, or has duplication. Use after completing a phase chunk, after `/audit` fixes, or when a file feels unwieldy. This is the complement to `/audit`: audit flags convention violations, refactor simplifies working code.

## Scope Determination

- **Whole kernel**: `/refactor` with no argument → scan entire `kernel/` tree
- **Directory**: `/refactor kernel/mm/` → scan all files in that directory
- **Single file**: `/refactor kernel/boot/limine.c` → scan only that file
- **Always skip**: `build/`, `vendor/`, `third_party/`, `kernel/include/`, vendored files (`limine.h`, `stb_*.h`)

## Behavioral Invariant

Every refactor MUST satisfy ALL five rules. If any is violated, exclude the refactor:

1. **Same output** — Serial output, return values, and side effects remain identical
2. **Same control flow** — No new or removed code paths (unless provably unreachable)
3. **Same API surface** — No function signature changes (except renaming `static` functions)
4. **No new features** — No new parameters, capabilities, or public functions
5. **No new dependencies** — No new `#include` directives

If you cannot prove a refactor preserves all five, do not propose it.

## Execution Flow

### Step 1: File Discovery

Glob the scope to collect all `.c`, `.h`, and `.asm` files. Exclude:
- `build/`
- `vendor/`, `third_party/`
- `kernel/include/` (vendored freestanding headers)
- Known vendored files: `limine.h`, `stb_*.h`

If zero files are found, report: **"No kernel source files found in scope. Nothing to refactor."** and stop.

Note which file types are present — if only `.asm` files exist, only Agents 1 and 4 apply.

### Step 2: Baseline Check

Verify the codebase is in a working state before touching anything:

1. **Build check**: Run `cmake --build build` (or the project's build command). If it fails, stop and report: **"Build is broken — fix build errors before refactoring. Cannot establish a clean baseline."**
2. **Boot test**: Run QEMU headless with a 10-second timeout, capture serial output, scan for `PANIC`, `TRIPLE FAULT`, `ASSERT`, or hang (no output). If the boot test fails, stop and report: **"Boot test failed — fix boot issues before refactoring. Cannot establish a clean baseline."**

The baseline serial output is saved for regression comparison.

### Step 3: Launch 5 Analysis Sub-Agents in Parallel

Use the **Agent tool** with `subagent_type: "general-purpose"` to launch all applicable agents **simultaneously** in a single message. Each sub-agent receives the file list and its check table.

If only `.asm` files exist, launch only Agents 1 and 4.

#### Agent 1: Dead Code & Redundancy (D01–D05)

| ID  | Check | Focus |
|-----|-------|-------|
| D01 | Unused local variables | Variables declared but never read |
| D02 | Unreachable code | Code after unconditional `return`/`goto`/`break`, `#if 0` blocks |
| D03 | Unused static functions | `static` functions never called within their translation unit |
| D04 | Redundant includes | Headers included but no symbol from them is used |
| D05 | Redundant operations | Assignments immediately overwritten, double initialization, no-op expressions |

#### Agent 2: Structural Complexity (X01–X05)

| ID  | Check | Focus |
|-----|-------|-------|
| X01 | Long functions (>60 lines) | Function body exceeds 60 lines — identify logical split points |
| X02 | Deep nesting (>3 levels) | More than 3 levels of `if`/`for`/`while`/`switch` — suggest early returns or extraction |
| X03 | Complex conditionals | Boolean expressions with 3+ terms, nested ternaries, compound conditions that need a named variable |
| X04 | Repeated code blocks | Near-identical code (>4 lines) appearing 2+ times — suggest extraction into a helper |
| X05 | Unnecessary indirection | Wrapper functions that just forward to another function, variables that alias another with no added value |

#### Agent 3: Magic Numbers (M01–M03)

| ID  | Check | Focus |
|-----|-------|-------|
| M01 | Magic numbers in logic | Numeric literals (other than 0, 1, -1) in control flow or arithmetic — should be named constants |
| M02 | Magic numbers in hardware code | Raw hex literals in I/O port operations, MMIO addresses, register offsets — should be `#define` constants |
| M03 | Hardcoded repeated strings | String literals used in 2+ places — should be a constant or shared definition |

#### Agent 4: Naming & Clarity (N01–N04)

| ID  | Check | Focus |
|-----|-------|-------|
| N01 | Single-letter variable names | Variables named `i`, `j`, `k` are OK for loop counters; flag all others (`p`, `x`, `n`, `s`, `t`) in non-trivial scopes (>10 lines) |
| N02 | Vague or misleading names | Names like `data`, `buf`, `tmp`, `ret`, `val`, `info`, `ctx` without a qualifying prefix — suggest more descriptive names |
| N03 | Inconsistent naming across files | Same concept named differently in different files (e.g., `frame` vs `page` vs `block` for physical pages) |
| N04 | Missing or stale comments | Commented-out code (not TODOs), comments that describe what the old code did (not current), doc comments that don't match the function signature |

#### Agent 5: Patterns & Consistency (P01–P04)

| ID  | Check | Focus |
|-----|-------|-------|
| P01 | Inconsistent error handling | Some functions return error codes, others use flags, others ignore errors — standardize within a subsystem |
| P02 | Inconsistent iteration patterns | Mixing `for` loop styles, index-based vs pointer-based iteration for the same data structure |
| P03 | Inconsistent initialization | Some structs zeroed with `= {0}`, others with `memset`, others field-by-field — pick one per subsystem |
| P04 | Copy-paste with minor variations | Code blocks that are 90%+ identical with only a value or field name changed — extract parameterized function |

---

## Sub-Agent Prompt Template

When launching each sub-agent, use this structure:

```
You are analyzing arc_os kernel source files for [CATEGORY NAME] refactoring opportunities.

## Files to Scan
[LIST OF FILE PATHS]

## Checks
[PASTE THE FULL CHECK TABLE FOR THIS AGENT]

## Behavioral Invariant
Every proposed refactor MUST preserve:
1. Same output (serial, return values, side effects)
2. Same control flow (no new/removed paths)
3. Same API surface (no signature changes except static renames)
4. No new features (no new params, capabilities, public functions)
5. No new dependencies (no new #include)

If you cannot prove a refactor is safe, do not propose it.

## Instructions
1. Use the Read tool to read each file listed above
2. Apply every check in the table to each file
3. For each finding, record:
   - Check ID
   - File path and line range
   - What the problem is (1-2 sentences)
   - Exact "before" code (copy verbatim from the file)
   - Exact "after" code (your proposed replacement)
   - Impact assessment: HIGH (bug risk, major readability gain), MEDIUM (moderate clarity improvement), LOW (cosmetic/minor)
4. Return findings sorted by impact (HIGH first)
5. If a file has zero findings, do not mention it
6. Be precise about line numbers — cite the exact lines
7. Return ONLY the findings list. If zero findings, return "No findings."
```

---

### Step 4: Aggregate & Prioritize

Collect all sub-agent results. Score each finding:

- **HIGH** — Reduces bug risk, eliminates significant duplication, or greatly improves readability of a core path
- **MEDIUM** — Moderate clarity improvement, removes minor redundancy
- **LOW** — Cosmetic naming, minor style consistency

Select the top 5–10 items (fewer if fewer findings exist — do not pad). Present the refactor plan:

```
## Refactor Plan: <scope>

**Files scanned**: <count>
**Total findings**: <count> across <agent_count> categories
**Selected for refactoring**: <selected_count> (top items by impact)

### #1 [HIGH] X01: Extract memory map parsing from boot_init()
**File**: `kernel/boot/limine.c:45-120`
**Why**: 75-line function with 4 levels of nesting makes the boot sequence hard to follow. Splitting isolates memory map logic for independent testing.

**Before**:
```c
// exact code from the file
```

**After**:
```c
// exact replacement code
```

**Test impact**: build + boot

### #2 [HIGH] D03: Remove unused static helper
**File**: `kernel/arch/x86_64/serial.c:88-95`
**Why**: `serial_check_parity()` is defined but never called — dead code adds confusion.

**Before**:
```c
static int serial_check_parity(uint16_t port) {
    return inb(port + 5) & 0x04;
}
```

**After**:
(delete entirely)

**Test impact**: build + boot

...
```

**Wait for user approval before applying.** The user may:
- Approve all items
- Approve a subset (e.g., "apply 1, 2, 5")
- Reject all and request adjustments
- Ask to modify a specific before/after

### Step 5: Apply Sequentially

For each approved item, in order:

1. **Verify the "before" code still matches** the file on disk. If the file has changed (e.g., from a prior refactor in this session), re-read it. If the before block no longer matches, skip the item and report: "Skipped #N — source changed, before block no longer matches."

2. **Apply the code change** using the Edit tool.

3. **Build check**: Run `cmake --build build`.
   - If build fails → revert with `git checkout -- <file>`, report: "Reverted #N — build failed: <error summary>", continue to next item.

4. **Unit test check** (if tests exist for the subsystem): Run the relevant test.
   - If test fails → revert with `git checkout -- <file>`, report: "Reverted #N — unit test failed: <error summary>", continue to next item.

5. **Boot test**: Run QEMU headless with 10-second timeout, capture serial output, scan for `PANIC`, `TRIPLE FAULT`, `ASSERT`, or no output (hang). Compare serial output to the baseline from Step 2.
   - If boot test fails → revert with `git checkout -- <file>`, report: "Reverted #N — boot test regression: <details>", continue to next item.

6. **Report success**: "Applied #N — build OK, boot OK"

If no test infrastructure exists (no unit tests, no boot test script), rely on build success only and note: "No boot test available — verified build only."

### Step 6: Summary

After all items are processed, present:

```
## Refactor Summary

**Applied**: <count>/<total> refactors
**Reverted**: <count> (build/test failures)
**Skipped**: <count> (source changed, user excluded)

### Applied
- #1 [HIGH] X01: Extract memory map parsing — build OK, boot OK
- #2 [HIGH] D03: Remove unused static — build OK, boot OK

### Reverted
- #4 [MEDIUM] N02: Rename `buf` → `serial_buffer` — build failed (used in macro expansion)

### Skipped
- #7 [LOW] M01: Replace magic number — user excluded

### Files Modified
- `kernel/boot/limine.c` (refactors #1, #3)
- `kernel/arch/x86_64/serial.c` (refactor #2)
```

---

## Difference from `/audit`

| Aspect | `/audit` | `/refactor` |
|--------|----------|-------------|
| Output | Violation report (ERROR/WARNING/INFO) | Prioritized plan with exact before/after code |
| Action | Flags problems | Fixes them one by one with testing |
| Testing | None (report only) | Build + boot test after each change |
| Focus | Convention compliance, safety, correctness | Simplification, readability, deduplication |
| Thresholds | 80-line functions, 4-level nesting | 60-line functions, 3-level nesting (stricter) |
| Auto-fix | Safe subset only (whitespace, guards) | All proposed changes (with user approval) |

## Edge Cases

- **No source files in scope**: Stop with message, do not launch agents.
- **Fewer than 5 findings**: Present all findings — do not pad to reach 5.
- **Only `.asm` files**: Run only Agents 1 and 4 — skip C-specific checks.
- **Build already failing**: Stop at Step 2. Report the build error and do not propose refactors.
- **Boot test failing at baseline**: Stop at Step 2. Report the boot failure and do not propose refactors.
- **Refactor causes build failure**: Revert with `git checkout -- <file>`, report failure, continue to next item.
- **Refactor causes boot regression**: Revert, report, continue.
- **File changed between plan and apply**: Re-read the file. If the "before" block no longer matches, skip and report.
- **No test infrastructure**: Skip unit tests and boot tests, rely on build success only. Note this in the summary.
- **User requests partial application**: Apply only the approved subset, in order.
- **Refactor would violate behavioral invariant**: Exclude it from the plan entirely — do not present it to the user.
