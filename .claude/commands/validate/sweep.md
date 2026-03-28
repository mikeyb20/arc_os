# /sweep — Focused Validation Sweep

## Instructions

You are running a focused validation sweep. Examine ONLY the specified
concern — do not mix concerns. A safety sweep does not comment on
architecture. A performance sweep does not flag naming issues.

## Step 0: Sweep Selection (if type not specified)

If the user hasn't specified which sweep type to run, recommend based on the
type of change:

| Change Type | Recommended Sweeps |
|------------|-------------------|
| New kernel subsystem | safety + architecture |
| Refactor | architecture + duplication |
| Performance-sensitive (hot paths) | performance |
| Multi-workstream merge | duplication |
| C kernel code | safety (always) |
| Bug fix only | safety (focused on fix area) |

Recommend 1-2 sweep types. Do NOT default to all 4.

## Step 1: Phase Detection (arc_os)

Read `docs/implementation-plan.md` and check `[x] DONE` markers:

- **Phases 0-2 active** → Run Agents 1, 2, 3, 4, 6 (skip 5 and 7)
- **Phase 3+ has any DONE chunk** → Also run Agent 5 (Concurrency)
- **Phase 5+ has any DONE chunk** → Also run Agent 7 (Security)
- **File missing or unparseable** → Run ALL 7 agents (conservative default)

## Step 2: File Discovery

Glob the scope to collect all `.c`, `.h`, and `.asm` files. Exclude:
- `build/`, `vendor/`, `third_party/`, `kernel/include/`
- Vendored files: `limine.h`, `stb_*.h`

Test files (`tests/`): Relaxed mode — boundary violations (A01) and missing
prefixes (S04) are INFO severity instead of ERROR/WARNING.

## Sweep Types

### safety
Focus: memory safety, undefined behavior, error handling completeness.

### architecture
Focus: module boundaries, dependency direction, abstraction quality.

### performance
Focus: unnecessary allocations, algorithmic complexity, cache behavior.

### duplication
Focus: repeated logic, near-duplicate code, extraction opportunities.

## arc_os Check Catalogs (7 Agent Categories)

### Agent 1: Style & Naming (S01-S13) — Always Active

| ID | Check | Severity |
|----|-------|----------|
| S01 | File naming: `snake_case` with `.c`/`.h`/`.asm` | ERROR |
| S02 | Header guards: `#ifndef ARCHOS_<PATH>_H` | ERROR |
| S03 | Function names: `snake_case` | ERROR |
| S04 | Function prefix: public functions use `subsystem_` prefix | WARNING |
| S05 | Type names: `PascalCase` or `snake_case_t` | WARNING |
| S06 | Macro/constant names: `UPPER_SNAKE_CASE` | WARNING |
| S07 | Doc comments on every `.h` declaration | WARNING |
| S08 | 4-space indentation, no tabs | ERROR |
| S09 | K&R brace style | WARNING |
| S10 | Assembly files under 100 lines | WARNING |
| S11 | `#pragma once` usage | WARNING |
| S12 | Trailing whitespace | INFO |
| S13 | Lines over 100 characters | WARNING |

### Agent 2: Architecture & Boundaries (A01-A08) — Always Active

Layer hierarchy: lib < boot < arch < mm < drivers < proc < fs/ipc/net < security

| ID | Check | Severity |
|----|-------|----------|
| A01 | Layer hierarchy violations (`#include` direction) | ERROR |
| A02 | No direct syscalls in userland except libc wrappers | ERROR |
| A03 | Circular include dependencies | ERROR |
| A04 | Platform-specific code outside `arch/` | ERROR |
| A05 | Forbidden libc calls in kernel code | ERROR |
| A06 | Direct Limine access outside boot code | ERROR |
| A07 | Missing includes (include what you use) | WARNING |
| A08 | Unused includes (include bloat) | INFO |

### Agent 3: Safety & Correctness (C01-C10) — Always Active

| ID | Check | Severity |
|----|-------|----------|
| C01 | Unchecked pointer dereference | WARNING |
| C02 | Unchecked return values | WARNING |
| C03 | Integer overflow in size calculations | WARNING |
| C04 | Signed/unsigned comparison mismatch | WARNING |
| C05 | Buffer access without bounds check | WARNING |
| C06 | Use-after-free patterns | ERROR |
| C07 | Uninitialized local variables | WARNING |
| C08 | Implicit fallthrough in switch cases | WARNING |
| C09 | Infinite loop without escape | INFO |
| C10 | Pointer arithmetic on `void*` | ERROR |

### Agent 4: Kernel-Specific Hazards (K01-K10) — Always Active

| ID | Check | Severity |
|----|-------|----------|
| K01 | Missing `volatile` on MMIO/hardware registers | ERROR |
| K02 | Magic numbers in hardware code | WARNING |
| K03 | Large stack allocations (>512 bytes, VLAs) | WARNING |
| K04 | Missing `__attribute__((packed))` on hardware structs | WARNING |
| K05 | Hardcoded kernel addresses | WARNING |
| K06 | Physical/virtual address confusion | WARNING |
| K07 | Assembly/C calling convention mismatch | ERROR |
| K08 | Missing `cli`/`sti` around critical hardware ops | WARNING |
| K09 | Busy-wait without timeout | INFO |
| K10 | Incorrect I/O port access width | WARNING |

### Agent 5: Concurrency (L01-L05) — Active from Phase 3+

| ID | Check | Severity |
|----|-------|----------|
| L01 | Shared mutable state without lock | ERROR |
| L02 | Lock ordering violations | WARNING |
| L03 | Holding lock across blocking operation | ERROR |
| L04 | Missing interrupt disable in spinlock | ERROR |
| L05 | Non-atomic read-modify-write on shared data | WARNING |

### Agent 6: Maintainability (M01-M07) — Always Active

| ID | Check | Severity |
|----|-------|----------|
| M01 | TODO/FIXME/HACK/XXX tracking | INFO |
| M02 | Functions over 80 lines | WARNING |
| M03 | Nesting depth over 4 levels | WARNING |
| M04 | Excessive global/static mutable variables | WARNING |
| M05 | Implementation code in headers | WARNING |
| M06 | Dead/unreachable code | INFO |
| M07 | Duplicate code blocks | INFO |

### Agent 7: Security (X01-X06) — Active from Phase 5+

| ID | Check | Severity |
|----|-------|----------|
| X01 | User pointer used without validation | ERROR |
| X02 | Kernel info leak to userspace | WARNING |
| X03 | Missing permission check | WARNING |
| X04 | TOCTOU race in path/resource access | WARNING |
| X05 | Integer truncation in syscall arguments | WARNING |
| X06 | Unbounded copy from user | ERROR |

## Output Format

```markdown
## <Sweep Type> Sweep: <scope description>

**Files scanned**: <count>
**Agents run**: <active_count>/7 (<deferred agents if any>)
**Findings**: <error_count> errors, <warning_count> warnings, <info_count> info

### ERRORS (<count>)
- [ID] file:line: description

### WARNINGS (<count>)
- [ID] file:line: description

### INFO (<count>)
- [ID] file:line: description

### Top Recommendations
1. <most impactful fix>
2. <second most impactful>
3. <third most impactful>
```

## Auto-Fix Offer

**Safe auto-fixes** (offer these):
- Tabs → 4 spaces (S08)
- Header guard rename (S02)
- Add doc comment stubs (S07)
- Trim trailing whitespace (S12)
- Replace unambiguous magic numbers with named constants (K02)

**Manual-review items** (NEVER auto-fix):
- Abstraction boundary violations (A01) — architectural decision
- Function/type renames (S03, S04, S05) — may break callers
- Missing `volatile` (K01) — requires understanding intent
- Concurrency issues (L01-L05), Security issues (X01-X06), Use-after-free (C06)
