# /done-check — Verify Done Criteria (Final Gate)

## Instructions

You are the final gate before merge. Every done criterion from the plan must
be verified. This runs AFTER code review — the code should already be correct.
Your job is to confirm completeness, not find bugs (that was /validate/review's job).

If any criterion fails, the feature is not ready to merge. Be rigorous.
"Probably works" is not a pass.

## Step 1: Load Criteria

Read the plan file and extract the "Done Criteria" section.
Each criterion should be a testable statement.

## Step 2: Verify Each Criterion

For each criterion:

1. **Determine how to verify it:**
   - Can it be checked by reading code? → read the relevant files
   - Can it be checked by running tests? → run them and report results
   - Can it be checked by running the application? → note it requires manual check
   - Can it be checked by inspecting git history? → check commits

2. **Execute the verification**

3. **Record the result:**
   - PASS — criterion is met, with evidence
   - FAIL — criterion is not met, with explanation of what's missing
   - MANUAL — requires human verification

## Step 3: arc_os Completeness Audit (F01-F13)

For each kernel subsystem in the changeset, check every artifact:

| ID  | Artifact | How to Check |
|-----|----------|-------------- |
| F01 | Header file exists | `kernel/<layer>/<name>.h` exists |
| F02 | Implementation file exists | `kernel/<layer>/<name>.c` exists |
| F03 | C source in build system | `.c` file listed in `KERNEL_C_SOURCES` in `kernel/CMakeLists.txt` |
| F04 | ASM source in build system | `.asm` file (if any) listed in `KERNEL_ASM_SOURCES` |
| F05 | Init call in kmain.c | `<name>_init(...)` called in `kernel/boot/kmain.c` |
| F06 | Init ordering correct | Init call appears after all dependency inits and before consumer inits |
| F07 | kprintf confirmation | Init function contains `kprintf("[<NAME>] ...")` message |
| F08 | Test file exists | `tests/test_<name>.c` exists |
| F09 | Test suite registered | `extern TestCase <name>_tests[]` and `extern int <name>_test_count` in `tests/test_main.c` |
| F10 | Test suite in Suite array | Entry in `suites[]` array in `tests/test_main.c` |
| F11 | Test file in CMake | `test_<name>.c` in `add_executable(test_runner ...)` in `tests/CMakeLists.txt` |
| F12 | CTest entry | `add_test(NAME test_<name> COMMAND test_runner --suite <name>)` in `tests/CMakeLists.txt` |
| F13 | Header included in kmain.c | `#include "<layer>/<name>.h"` at top of `kernel/boot/kmain.c` |

Items that don't apply (e.g., F04 when there's no .asm file) count as PASS, not missing.

## Step 4: Test Quality Audit (T01-T10)

For each test file in the changeset:

| ID  | Check | Severity |
|-----|-------|----------|
| T01 | Minimum test count | At least 8 test cases (ERROR) |
| T02 | Failure path coverage | At least one test exercises allocation failure (ERROR) |
| T03 | Static stubs | All stub functions declared `static` (ERROR) |
| T04 | Header guard pattern | Kernel headers guarded with `#define ARCHOS_<PATH>_H` (WARNING) |
| T05 | Type reproduction | Structs/enums from guarded headers reproduced with matching fields (WARNING) |
| T06 | Include pattern | Implementation included as `#include "../kernel/<layer>/<name>.c"` (ERROR) |
| T07 | Suite export | Exports `TestCase <name>_tests[]` and `int <name>_test_count` (ERROR) |
| T08 | No kernel/include | Test file does NOT include from `kernel/include/` (ERROR) |
| T09 | Failure injection | kmalloc stub has force-fail mechanism (WARNING) |
| T10 | State reset function | `reset_<name>_state()` exists (WARNING) |

## Step 5: Run Full Test Suite

Execute the project's test command. Report:
- Total tests: <count>
- Passing: <count>
- Failing: <count> (with details)
- Skipped: <count>

## Step 6: Report

```markdown
## Done Check: <feature name> (Final Gate)

### Plan Done Criteria
| # | Criterion | Status | Evidence |
|---|-----------|--------|----------|
| 1 | <criterion> | PASS | <evidence> |
| 2 | <criterion> | FAIL | <what's missing> |

### Completeness by Subsystem
#### <subsystem> (e.g., `drivers/pci`)
| Artifact | Status |
|----------|--------|
| F01 Header | PASS |
| F02 Implementation | PASS |
| ... | ... |

**Result**: <N>/13 — COMPLETE / NEEDS REMEDIATION

### Test Quality
| ID | Check | Status |
|----|-------|--------|
| T01 | Min test count | PASS (12 tests) |
| ... | ... | ... |

### Test Suite
<pass/fail summary>

### Verdict: <READY TO MERGE / NOT READY>

### Remaining Items (if not ready)
<specific list of what needs to happen before re-running done-check>
```

## Remediation Plan

If verdict is NOT READY, present a prioritized fix list:

| Priority | ID | Category | Description | Action |
|----------|----|----------|-------------|--------|
| P1 | F08 | GAP | Missing test file | Create tests |
| P2 | S02 | VIOLATION | Wrong header guard | Fix guard |
| P3 | T01 | TEST | Only 6 tests | Add 2+ more |

- **P1**: Gaps — missing required artifacts. Must fix.
- **P2**: Errors — convention violations or integration issues.
- **P3**: Warnings — quality gaps that should be fixed.
