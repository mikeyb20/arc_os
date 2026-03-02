# verify-changes

Post-implementation audit focused on the delta — what just changed. Checks completeness (missing files, tests, init calls), convention compliance, test quality, and integration correctness. Produces a prioritized remediation plan. This is the third step in the plan → implement → verify lifecycle.

## When to Use

Invoke after completing a feature implementation, after `/guide-implementation` reports all steps DONE, before committing, or as a final quality gate before marking a phase chunk complete. Unlike `/audit-code` which scans the entire codebase for convention violations, this skill focuses on **completeness** of the recent change and **delta-scoped** convention checks.

## Scope Determination

- **Auto-detect**: `/verify-changes` with no argument → detect changed files from `git diff` (staged, unstaged, and recent commits)
- **Specific commit range**: `/verify-changes HEAD~3..HEAD` → verify changes in the last 3 commits
- **Specific files**: `/verify-changes kernel/drivers/pci.c kernel/drivers/pci.h` → verify only those files

---

## Execution Flow

### Step 1: Determine Changed Files

Detect the changeset using git:

1. Check for unstaged changes: `git diff --name-only`
2. Check for staged changes: `git diff --cached --name-only`
3. If no unstaged or staged changes, check recent commits: `git diff --name-only HEAD~1..HEAD`
4. If the user provided a commit range, use that instead

Combine all changed files into a single list. Categorize each file:

| Category | Pattern | Example |
|----------|---------|---------|
| Kernel header | `kernel/**/*.h` | `kernel/drivers/pci.h` |
| Kernel impl | `kernel/**/*.c` (not `kmain.c`) | `kernel/drivers/pci.c` |
| Kernel assembly | `kernel/**/*.asm` | `kernel/arch/x86_64/pci_access.asm` |
| Build system | `kernel/CMakeLists.txt` | — |
| Init file | `kernel/boot/kmain.c` | — |
| Test file | `tests/test_*.c` | `tests/test_pci.c` |
| Test infra | `tests/test_main.c`, `tests/CMakeLists.txt` | — |
| Other | Anything else | `docs/`, `tools/`, etc. |

Identify **subsystems** in the changeset by grouping kernel files by directory (e.g., all files under `kernel/drivers/` = one subsystem, all files under `kernel/mm/` = another).

If zero files are found, report: **"No changes detected. Nothing to verify."** and stop.

### Step 2: Launch 4 Parallel Sub-Agents

Use the **Agent tool** with `subagent_type: "general-purpose"` to launch all four agents **simultaneously** in a single message.

#### Agent 1: Completeness Audit

```
You are auditing arc_os kernel changes for completeness — checking that every
subsystem in the changeset has all required artifacts.

## Changed Files
[LIST OF CHANGED FILE PATHS WITH CATEGORIES]

## Subsystems in Changeset
[LIST OF SUBSYSTEMS IDENTIFIED]

## Completeness Checklist (check each for every subsystem)

| ID  | Artifact | How to Check |
|-----|----------|-------------- |
| F01 | Header file exists | `kernel/<layer>/<name>.h` exists |
| F02 | Implementation file exists | `kernel/<layer>/<name>.c` exists |
| F03 | C source in build system | `.c` file listed in `KERNEL_C_SOURCES` in `kernel/CMakeLists.txt` |
| F04 | ASM source in build system | `.asm` file (if any) listed in `KERNEL_ASM_SOURCES` in `kernel/CMakeLists.txt` |
| F05 | Init call in kmain.c | `<name>_init(...)` called in `kernel/boot/kmain.c` |
| F06 | Init ordering correct | Init call appears after all dependency inits and before consumer inits |
| F07 | kprintf confirmation | Init function contains `kprintf("[<NAME>] ...")` message |
| F08 | Test file exists | `tests/test_<name>.c` exists |
| F09 | Test suite registered | `extern TestCase <name>_tests[]` and `extern int <name>_test_count` in `tests/test_main.c` |
| F10 | Test suite in Suite array | Entry in the `suites[]` array in `tests/test_main.c` |
| F11 | Test file in CMake | `test_<name>.c` in `add_executable(test_runner ...)` in `tests/CMakeLists.txt` |
| F12 | CTest entry | `add_test(NAME test_<name> COMMAND test_runner --suite <name>)` in `tests/CMakeLists.txt` |
| F13 | Header included in kmain.c | `#include "<layer>/<name>.h"` at top of `kernel/boot/kmain.c` |

## Instructions
1. Read `kernel/CMakeLists.txt`, `kernel/boot/kmain.c`, `tests/test_main.c`, `tests/CMakeLists.txt`
2. For each subsystem, read its header and implementation files
3. Check every item in the checklist
4. For F06 (init ordering), verify the layer hierarchy:
   lib < boot < arch < mm < drivers < proc < fs/ipc/net < security
   A subsystem's init must come after all subsystems it depends on
5. Report each missing artifact as: `- [ID] <subsystem>: <description of what's missing>`
6. If all artifacts exist for a subsystem, report: `- <subsystem>: COMPLETE (13/13)`
7. Items that don't apply (e.g., F04 when there's no .asm file) count as PASS, not missing
```

#### Agent 2: Convention Compliance (Delta-Scoped)

```
You are auditing arc_os kernel changes for convention compliance.
Only scan the CHANGED FILES listed below — not the entire codebase.

## Changed Files
[LIST OF CHANGED FILE PATHS]

## Checks (subset of /audit-code, focused on most impactful)

| ID  | Check | Severity |
|-----|-------|----------|
| S01 | File naming: snake_case with .c/.h/.asm extension | ERROR |
| S02 | Header guards: `#ifndef ARCHOS_<PATH>_H` / `#define ARCHOS_<PATH>_H` | ERROR |
| S03 | Function names: snake_case | ERROR |
| S04 | Function prefix: public functions use subsystem_ prefix | WARNING |
| S07 | Doc comments on every .h declaration | WARNING |
| S08 | 4-space indentation, no tabs | ERROR |
| S09 | K&R brace style | WARNING |
| A01 | Layer hierarchy violations (#include direction) | ERROR |
| A04 | Platform-specific code outside arch/ | ERROR |
| A05 | Forbidden libc calls in kernel code (printf, malloc, free) | ERROR |
| C01 | Unchecked pointer dereference after fallible function | WARNING |
| C02 | Unchecked return values from functions that can fail | WARNING |
| C06 | Use-after-free patterns | ERROR |
| C10 | Pointer arithmetic on void* | ERROR |
| K03 | Large stack allocations (>512 bytes, VLAs) | WARNING |
| K05 | Hardcoded kernel addresses outside config headers | WARNING |

## Instructions
1. Read each changed file using the Read tool
2. Apply every check to each file
3. Report findings as: `- [ID] file:line: description` grouped by severity
4. If zero findings, return "No convention violations found."
```

#### Agent 3: Test Quality

```
You are auditing the test quality of recently changed or created test files
in the arc_os project.

## Test Files to Audit
[LIST OF test_*.c FILES FROM THE CHANGESET, OR test files corresponding to changed subsystems]

## Test Quality Checks

| ID  | Check | What to Look For |
|-----|-------|------------------|
| T01 | Minimum test count | At least 8 test cases in the TestCase array |
| T02 | Failure path coverage | At least one test exercises allocation failure (NULL return from kmalloc or equivalent) |
| T03 | Static stubs | All stub functions (kmalloc, kfree, kprintf, etc.) are declared `static` to avoid linker clashes |
| T04 | Header guard pattern | Kernel headers are guarded with `#define ARCHOS_<PATH>_H` before any includes, preventing conflicts with system headers |
| T05 | Type reproduction | Structs and enums from guarded headers are reproduced in the test file with matching field names and types |
| T06 | Include pattern | Implementation included as `#include "../kernel/<layer>/<name>.c"` (includes the .c file directly) |
| T07 | Suite export | File exports `TestCase <name>_tests[]` array and `int <name>_test_count` variable |
| T08 | No kernel/include | Test file does NOT `#include` anything from `kernel/include/` (freestanding headers conflict with system headers) |
| T09 | Failure injection | kmalloc stub (or equivalent) has a force-fail mechanism (e.g., `kmalloc_force_fail` counter) |
| T10 | State reset function | A `reset_<name>_state()` function exists that cleans up all module-level state between tests |

## Instructions
1. Read each test file using the Read tool
2. Apply every check
3. For T05, compare reproduced types against the actual kernel header to verify they match
4. Report findings as: `- [ID] file:line: description` with severity (ERROR for T01-T03, T06-T08; WARNING for T04-T05, T09-T10)
5. If a test file doesn't exist for a changed subsystem, report: `- [T00] <subsystem>: No test file found`
```

#### Agent 4: Integration

```
You are checking integration correctness of recent arc_os kernel changes.

## Changed Files
[LIST OF CHANGED FILE PATHS]

## Integration Checks

| ID  | Check | What to Look For |
|-----|-------|------------------|
| I01 | Init order correctness | If a new init call was added to kmain.c, verify it respects the layer hierarchy: lib < boot < arch < mm < drivers < proc < fs/ipc/net < security. The new init must come AFTER all subsystems it depends on and BEFORE all subsystems that depend on it. Read kmain.c and trace #include dependencies. |
| I02 | No circular includes | Trace #include chains in changed headers. A.h must not transitively include itself. Read each changed header and follow its includes. |
| I03 | Consistent types | Types used across file boundaries must match exactly. If a struct is defined in one file and used in another, check field names, types, and sizes match. Pay special attention to types reproduced in test files (T05). |
| I04 | Spinlock usage for ISR-shared state | If any changed file has global/static mutable state accessed from both ISR context and normal context, it must be protected by a spinlock with interrupts disabled (cli). |
| I05 | EOI ordering | If any changed ISR handler code exists, verify that `pic_send_eoi()` is called BEFORE the handler logic (the handler may context_switch away and never return to send EOI). Reference: `kernel/arch/x86_64/isr.c` pattern. |
| I06 | Thread safety for global state | If any changed file adds new global/static mutable variables, check if they could be accessed from multiple threads. If so, synchronization is needed (or a comment explaining why it's safe). |

## Instructions
1. Read `kernel/boot/kmain.c` for init ordering (I01)
2. Read each changed header and trace includes for circularity (I02)
3. Read changed files and their corresponding headers/test files for type consistency (I03)
4. Read changed files for global/static mutable state and check for ISR interaction (I04, I06)
5. Read any changed ISR-related code for EOI ordering (I05)
6. Report findings as: `- [ID] file:line: description`
7. For I01, show the expected init order and the actual order
```

### Step 3: Classify Findings

Collect all sub-agent results and classify each finding:

| Category | Source | Meaning |
|----------|--------|---------|
| **GAP** | Agent 1 | Missing artifact (file, build entry, test, init call) |
| **VIOLATION** | Agent 2 | Convention broken in changed code |
| **TEST-ISSUE** | Agent 3 | Test quality problem (insufficient tests, missing stubs, pattern violation) |
| **INTEGRATION** | Agent 4 | Integration correctness problem (init order, circular includes, type mismatch, thread safety) |

### Step 4: Produce Report

```
## Verification Report: <scope description>

**Changed files**: <count>
**Subsystems affected**: <list>
**Checks run**: Completeness (13), Convention (16), Test Quality (10), Integration (6)

### Completeness by Subsystem

#### <subsystem 1> (e.g., `drivers/pci`)
| Artifact | Status |
|----------|--------|
| F01 Header | PASS |
| F02 Implementation | PASS |
| F03 C source in CMake | PASS |
| F04 ASM source in CMake | N/A |
| F05 Init call | PASS |
| F06 Init ordering | PASS |
| F07 kprintf confirmation | PASS |
| F08 Test file | **MISSING** |
| F09 Test registered | **MISSING** |
| F10 Suite in array | **MISSING** |
| F11 Test in CMake | **MISSING** |
| F12 CTest entry | **MISSING** |
| F13 Header in kmain.c | PASS |

**Result**: 8/13 — NEEDS REMEDIATION (missing test infrastructure)

### Convention Violations (<count>)
- [S02] kernel/drivers/pci.h:1: Header guard uses `PCI_H` — should be `ARCHOS_DRIVERS_PCI_H`
- [A01] kernel/drivers/pci.c:5: Includes `kernel/proc/sched.h` — drivers cannot depend on proc

### Test Quality Issues (<count>)
- [T00] drivers/pci: No test file found
- [T01] tests/test_sched.c: Only 6 tests — minimum is 8

### Integration Issues (<count>)
- [I01] kernel/boot/kmain.c:115: `pci_init()` called before `kmalloc_init()` — PCI needs heap allocation

### Overall Verdict

**PASS** — All checks pass, all artifacts present
**PASS WITH WARNINGS** — All artifacts present, minor convention issues (warnings only)
**NEEDS REMEDIATION** — Missing artifacts or errors found (see remediation plan)
```

### Step 5: Offer Remediation

If verdict is NEEDS REMEDIATION or PASS WITH WARNINGS, present a prioritized fix list:

```
### Remediation Plan

| Priority | ID | Category | Description | Action |
|----------|----|----------|-------------|--------|
| P1 | F08 | GAP | Missing test file for `drivers/pci` | Create `tests/test_pci.c` with 8+ tests |
| P1 | F09 | GAP | Test suite not registered | Add extern decls to `tests/test_main.c` |
| P1 | F10 | GAP | Suite not in array | Add entry to `suites[]` in `tests/test_main.c` |
| P1 | F11 | GAP | Test not in CMake | Add `test_pci.c` to `add_executable` in `tests/CMakeLists.txt` |
| P1 | F12 | GAP | No CTest entry | Add `add_test(NAME test_pci ...)` to `tests/CMakeLists.txt` |
| P2 | S02 | VIOLATION | Wrong header guard format | Fix guard in `kernel/drivers/pci.h` |
| P2 | A01 | VIOLATION | Layer hierarchy violation | Remove `#include "proc/sched.h"` from `kernel/drivers/pci.c` |
| P2 | I01 | INTEGRATION | Init order wrong | Move `pci_init()` after `kmalloc_init()` in kmain.c |
| P3 | T01 | TEST-ISSUE | Only 6 tests in sched suite | Add 2+ more tests to `tests/test_sched.c` |
| P4 | S09 | VIOLATION | K&R brace style violation | Fix brace placement (line 42) |

**Priority levels**:
- **P1**: Gaps — missing required artifacts. Must fix before feature is complete.
- **P2**: Errors — convention violations or integration issues that could cause bugs.
- **P3**: Warnings — convention issues or test quality gaps that should be fixed.
- **P4**: Info — minor style issues, cosmetic improvements.
```

After presenting the remediation plan, offer to fix items in priority order. Apply fixes sequentially, building after each kernel file change.

---

## Difference from `/audit-code`

| Aspect | `/audit-code` | `/verify-changes` |
|--------|---------------|-------------------|
| Scope | Entire codebase or specified directory | Delta only — recently changed files |
| Focus | Convention violations (style, safety, arch) | **Completeness** (missing artifacts) + conventions |
| Completeness checks | None | 13-point checklist per subsystem |
| Test quality checks | None | 10-point test quality audit |
| Integration checks | Boundary violations only (A01) | 6 integration checks (init order, circular includes, types, concurrency) |
| Output | Violation report | Remediation plan with priorities |
| Auto-fix | Safe subset only | All fixes offered in priority order |

---

## Edge Cases

- **No changes detected**: Report "No changes detected. Nothing to verify." and stop.
- **Only test files changed**: Skip Agent 1 (completeness) and Agent 4 (integration). Run Agents 2 and 3 only.
- **Only build system changed**: Run Agent 1 (completeness) to verify the build entries match existing files. Skip Agents 2-4.
- **New subsystem with no tests yet**: Agent 1 reports F08-F12 as MISSING. Agent 3 reports T00. Verdict is NEEDS REMEDIATION.
- **Extension to existing subsystem**: Agent 1 checks completeness against the existing subsystem's artifacts (they should already exist). Focus shifts to whether new functions have test coverage.
- **Changes span multiple subsystems**: Run the completeness checklist independently for each subsystem. Report per-subsystem results.
- **Vendor or third-party files changed**: Skip convention checks (Agent 2) for files in `vendor/`, `third_party/`, `kernel/include/`. Still check completeness and integration.
- **Git state is dirty with mixed changes**: Combine staged and unstaged changes. Warn the user: "Mixed staged/unstaged changes detected — verifying all changes together."
- **All checks pass**: Report "PASS — all artifacts present, no violations found" with the full completeness table showing all PASS entries.

## Integration with Lifecycle

- **Input from**: `/guide-implementation` — runs after all 8 implementation steps are DONE
- **Also standalone**: Can be invoked independently on any changeset
- **Feeds back to**: `/guide-implementation` — if gaps are found, the remediation plan maps back to implementation steps
- **Convention alignment**: Uses the same check IDs as `/audit-code` (S01, A01, C06, etc.) for consistency
- **Test quality checks**: Derived from the test patterns established in `tests/test_thread.c` and documented in project memory
