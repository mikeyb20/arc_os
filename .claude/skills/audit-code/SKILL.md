# audit-code

Scan kernel source files against arc_os conventions from CLAUDE.md. Produce a structured report with ERROR/WARNING/INFO severities.

## When to Use

Invoke after writing or modifying kernel code to verify it follows project conventions. Use before committing, after `/scaffold`, or as a quality gate before marking a phase complete.

## Scope Determination

- **Specific subsystem**: `/audit pmm` → scan `kernel/mm/pmm.c`, `kernel/mm/pmm.h`, related files
- **Directory**: `/audit kernel/arch/x86_64/` → scan all files in that directory
- **Whole kernel**: `/audit` with no argument → scan entire `kernel/` tree
- **Always skip**: `build/`, vendored files (e.g., `limine.h`, `stb_*.h`), generated files

## Convention Checks

### Check 1: File Naming (ERROR)
All source files must be `snake_case` with extensions `.c`, `.h`, or `.asm`.
- **Pass**: `pmm.c`, `boot_info.h`, `isr_stubs.asm`
- **Fail**: `PMM.c`, `bootInfo.h`, `isrStubs.asm`, `pmm.cpp`

### Check 2: Header Guards (ERROR)
Format: `#ifndef ARCHOS_<PATH>_H` / `#define ARCHOS_<PATH>_H` where `<PATH>` is the path from `kernel/` with `/` replaced by `_`, all uppercase.
- `kernel/mm/pmm.h` → `ARCHOS_MM_PMM_H`
- `kernel/arch/x86_64/gdt.h` → `ARCHOS_ARCH_X86_64_GDT_H`
- `kernel/lib/kprintf.h` → `ARCHOS_LIB_KPRINTF_H`

### Check 3: Function Names (ERROR / WARNING)
- **ERROR**: Non-`snake_case` function names (e.g., `allocPage`, `InitGdt`)
- **WARNING**: Missing subsystem prefix — public functions should start with `<subsystem>_` (e.g., `pmm_alloc_page`, not `alloc_page`). Static/internal functions are exempt from prefix requirement.

### Check 4: Type Names (WARNING)
Types and structs should be `PascalCase` or `snake_case_t`.
- **Pass**: `BootInfo`, `thread_t`, `PageTable`
- **Fail**: `boot_info` (no `_t` suffix and not PascalCase), `BOOT_INFO` (that's a macro name)

### Check 5: Constant/Macro Names (WARNING)
`#define` constants and macros should be `UPPER_SNAKE_CASE`.
- **Pass**: `PAGE_SIZE`, `GFP_KERNEL`, `MAX_CPUS`
- **Fail**: `pageSize`, `Page_Size`
- **Exempt**: Include guards (checked separately), function-like macros that wrap expressions

### Check 6: Doc Comments on Public API (WARNING)
Every non-static function declared in a `.h` file should have a doc comment (a `/*` or `//` comment on the line(s) immediately preceding the declaration).

### Check 7: Abstraction Boundary Violations (ERROR)
The layer hierarchy (bottom to top):
```
lib (bottom — no kernel dependencies)
  ↑
boot (parses bootloader info)
  ↑
arch (HAL implementation — GDT, IDT, PIC, etc.)
  ↑
mm (memory management — uses HAL)
  ↑
drivers (device drivers — uses HAL + mm)
  ↑
proc (processes, scheduler — uses mm + drivers)
  ↑
fs / ipc / net (higher services — uses proc + mm)
  ↑
security (top — policy over all below)
```

**Rules**:
- A layer may only `#include` headers from its own layer or layers below it
- **Illegal patterns** (examples):
  - `kernel/mm/*.c` including `kernel/proc/*.h` (mm must not depend on proc)
  - `kernel/arch/*.c` including `kernel/fs/*.h` (arch must not depend on fs)
  - `kernel/lib/*.c` including any other `kernel/` headers except `<stdint.h>` etc.
  - `kernel/boot/*.c` including `kernel/mm/*.h` (boot must not depend on mm)
- **Legal patterns**:
  - `kernel/mm/*.c` including `kernel/arch/hal.h` (mm depends on HAL)
  - `kernel/proc/*.c` including `kernel/mm/vmm.h` (proc depends on mm)
  - Any layer including `kernel/lib/*.h` (lib is the bottom layer)

### Check 8: Indentation (ERROR for tabs)
- 4-space indentation required
- Tabs are an ERROR (except in Makefiles or `.asm` files where tabs may be conventional)
- Mixed tabs+spaces on the same line is always an ERROR

### Check 9: Brace Style (WARNING)
K&R style: opening brace on the same line as the statement.
- **Pass**: `void foo(void) {`, `if (x) {`, `struct Foo {`
- **Fail**: Opening brace on its own line (Allman style)

### Check 10: Assembly File Length (WARNING)
`.asm` files should be under 100 lines. If longer, suggest splitting.

### Check 11: No Direct Syscalls from Userland (ERROR)
Files in `userland/` or `libc/` must not contain inline `syscall` / `int 0x80` / `sysenter` instructions. They should call libc wrappers instead. (Check `libc/` source for raw syscall wrappers is OK — that's where they belong.)

## Report Format

```
## Audit Report: <scope>

**Files scanned**: <count>
**Compliance rate**: <pass_count>/<total_checks> (<percentage>%)

### ERRORS (<count>)
- [E01] kernel/mm/pmm.c: Header guard is `PMM_H`, expected `ARCHOS_MM_PMM_H`
- [E07] kernel/mm/pmm.c:12: Includes `kernel/proc/scheduler.h` — mm layer cannot depend on proc

### WARNINGS (<count>)
- [W03] kernel/mm/pmm.c:25: Function `alloc_page()` missing subsystem prefix, expected `pmm_alloc_page()`
- [W06] kernel/mm/pmm.h:10: `pmm_alloc_page()` has no doc comment

### INFO (<count>)
- [I] kernel/arch/x86_64/isr_stubs.asm: 87 lines (under 100 limit)
- [I] tests/test_pmm.c: Test file — relaxed rules applied

### Top Recommendations
1. Fix all ERRORs before committing — these are convention violations
2. <specific actionable fix>
3. <specific actionable fix>
```

## Auto-Fix Offer

After reporting, offer to auto-fix **safe** changes:
- **Tabs → 4 spaces** (safe — purely whitespace)
- **Header guard rename** (safe — mechanical replacement)
- **Add doc comment stubs** above undocumented public API functions (safe — adds `/* TODO: document */`)

**Manual-review items** (do NOT auto-fix):
- Abstraction boundary violations (requires architectural decision)
- Function renames (may break callers)
- Type renames (may break callers)
- Brace style changes (risk of breaking multi-line macros)

## Edge Cases

- **No source code yet**: Report "No kernel source files found. Project is in Phase 0 — run `/scaffold` to create initial subsystem files."
- **Only assembly files**: Skip C-specific checks (header guards, doc comments, type names). Still check: file naming, length, indentation.
- **Test files** (`tests/`): Apply relaxed rules — boundary violations are INFO not ERROR (tests may include anything), missing prefixes are INFO.
- **Vendored/external files**: Skip entirely. Detect by path (`vendor/`, `third_party/`) or known vendored filenames (`limine.h`, `stb_*.h`).
- **Headers with `#pragma once`**: Flag as WARNING — project convention is `#ifndef` guards, not `#pragma once`.
