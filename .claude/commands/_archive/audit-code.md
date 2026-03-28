# audit-code

Scan kernel source files against arc_os conventions, safety rules, and kernel-specific hazards. Produce a structured report with ERROR/WARNING/INFO severities. Runs 7 check categories as **parallel sub-agents** for speed.

## When to Use

Invoke after writing or modifying kernel code to verify it follows project conventions. Use before committing, after `/scaffold`, or as a quality gate before marking a phase complete.

## Scope Determination

- **Specific subsystem**: `/audit pmm` → scan `kernel/mm/pmm.c`, `kernel/mm/pmm.h`, related files
- **Directory**: `/audit kernel/arch/x86_64/` → scan all files in that directory
- **Whole kernel**: `/audit` with no argument → scan entire `kernel/` tree
- **Always skip**: `build/`, vendored files (`vendor/`, `third_party/`, `kernel/include/`, `limine.h`, `stb_*.h`), generated files

## Execution Flow

### Step 1: Phase Detection

Read `docs/implementation-plan.md` and look for `[x] DONE` markers on phase chunks:

- **Phases 0-2 active** → Run Agents 1, 2, 3, 4, 6 (skip 5 and 7)
- **Phase 3+ has any DONE chunk** → Also run Agent 5 (Concurrency)
- **Phase 5+ has any DONE chunk** → Also run Agent 7 (Security)
- **File missing or unparseable** → Run ALL 7 agents (conservative default)

Print which agents are active and which are deferred before launching.

### Step 2: File Discovery

Glob the scope to collect all `.c`, `.h`, and `.asm` files. Exclude:
- `build/`
- `vendor/`, `third_party/`
- `kernel/include/` (vendored freestanding headers)
- Known vendored files: `limine.h`, `stb_*.h`

If zero files are found, report: **"No kernel source files found. Run `/scaffold` first."** and stop.

If only `.asm` files are found, run only Agents 1, 4, 6 (skip C-specific checks).

### Step 3: Launch Sub-Agents in Parallel

Use the **Task tool** with `subagent_type: "general-purpose"` to launch each active agent category **simultaneously**. Each sub-agent receives:
1. The list of file paths to scan
2. Its check table (IDs, patterns, severities)
3. Instructions to read each file and report findings in the format: `[ID] file:line: description`

Launch all active agents in a **single message** with multiple Task tool calls so they run in parallel.

Each sub-agent prompt must include:
- The full check table for that category (copy the relevant section below)
- The file list to scan
- Instructions to read each file using the Read tool
- Instructions to return findings as a structured list: `- [ID] path/file.c:LINE: description` grouped by severity (ERROR, WARNING, INFO)
- Note which files are in `tests/` (relaxed mode — see Edge Cases)

### Step 4: Aggregate Results

Collect all sub-agent results. Merge findings into a single report sorted by severity (ERRORS first, then WARNINGS, then INFO). Deduplicate any overlapping findings. Format per the Report Format section below.

### Step 5: Auto-Fix Offer

After presenting the report, offer to auto-fix safe changes (see Auto-Fix section).

---

## Check Catalog

### Agent 1: Style & Naming (S01–S13) — Always Active

| ID | Check | Severity | Pattern |
|----|-------|----------|---------|
| S01 | File naming: `snake_case` with `.c`/`.h`/`.asm` extension only | ERROR | `PMM.c`, `bootInfo.h`, `isrStubs.asm`, `pmm.cpp` |
| S02 | Header guards: `#ifndef ARCHOS_<PATH>_H` / `#define ARCHOS_<PATH>_H` format | ERROR | Path from `kernel/` with `/` → `_`, all uppercase. `kernel/mm/pmm.h` → `ARCHOS_MM_PMM_H`. `kernel/arch/x86_64/gdt.h` → `ARCHOS_ARCH_X86_64_GDT_H` |
| S03 | Function names: `snake_case` | ERROR | Fail: `allocPage`, `InitGdt`. Pass: `pmm_alloc_page`, `gdt_init` |
| S04 | Function prefix: public (non-static) functions use `subsystem_` prefix | WARNING | `alloc_page()` → should be `pmm_alloc_page()`. Static/internal functions exempt |
| S05 | Type names: `PascalCase` or `snake_case_t` | WARNING | Fail: `boot_info` (no `_t`), `BOOT_INFO`. Pass: `BootInfo`, `thread_t` |
| S06 | Macro/constant names: `UPPER_SNAKE_CASE` | WARNING | Fail: `pageSize`, `Page_Size`. Exempt: include guards, function-like macros |
| S07 | Doc comments on every `.h` declaration | WARNING | Every non-static function in `.h` needs `/*` or `//` comment on preceding line(s) |
| S08 | 4-space indentation, no tabs | ERROR | Tabs are ERROR (except in `.asm` files). Mixed tabs+spaces always ERROR |
| S09 | K&R brace style (opening brace on same line) | WARNING | Pass: `void foo(void) {`. Fail: brace on its own line (Allman style) |
| S10 | Assembly files under 100 lines | WARNING | `.asm` files over 100 lines — suggest splitting |
| S11 | `#pragma once` usage | WARNING | Project convention is `#ifndef` guards, not `#pragma once` |
| S12 | Trailing whitespace on lines | INFO | Any line ending with spaces or tabs before newline |
| S13 | Lines over 100 characters | WARNING | Any line exceeding 100 columns |

### Agent 2: Architecture & Boundaries (A01–A08) — Always Active

The layer hierarchy (bottom to top):
```
lib (bottom — no kernel dependencies)
  ↑
boot (parses bootloader info)
  ↑
arch (HAL — GDT, IDT, PIC, etc.)
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

| ID | Check | Severity | Pattern |
|----|-------|----------|---------|
| A01 | Layer hierarchy violations (`#include` direction) | ERROR | A layer including headers from a layer above it. Examples: `kernel/mm/*.c` including `kernel/proc/*.h`, `kernel/arch/*.c` including `kernel/fs/*.h`, `kernel/lib/*.c` including other kernel headers, `kernel/boot/*.c` including `kernel/mm/*.h` |
| A02 | No direct syscalls in userland/libc (except libc wrappers) | ERROR | `syscall`/`int 0x80`/`sysenter` instructions in `userland/` files. In `libc/` source, raw syscall wrappers are OK |
| A03 | Circular include dependencies | ERROR | `A.h` includes `B.h` which includes `A.h` (directly or transitively) |
| A04 | Platform-specific code outside `arch/` | ERROR | x86 instructions (`__asm__`, `asm(`, `inb`, `outb`, `outw`, `outl`, `cpuid`, `rdmsr`, `wrmsr`) in files outside `kernel/arch/` and `kernel/boot/` |
| A05 | Forbidden libc calls in kernel code | ERROR | `printf(`, `malloc(`, `free(`, `exit(`, `abort(`, `calloc(`, `realloc(` in kernel code — must use kernel equivalents (`kprintf`, `kmalloc`, `kfree`, etc.) |
| A06 | Direct Limine access outside boot code | ERROR | `#include <limine.h>` or `#include "limine.h"` or `limine_` prefixed symbols in files outside `kernel/boot/limine_requests.c` and `kernel/boot/limine.c` |
| A07 | Include what you use — missing includes | WARNING | Using types/functions without including their declaring header |
| A08 | Unused includes (include bloat) | INFO | Header included but no symbol from it appears to be used in the file |

### Agent 3: Safety & Correctness (C01–C10) — Always Active

| ID | Check | Severity | Pattern |
|----|-------|----------|---------|
| C01 | Unchecked pointer dereference | WARNING | Pointer used without null check after a function that can return NULL (`kmalloc`, `pmm_alloc_page`, etc.) |
| C02 | Unchecked return values | WARNING | Ignoring return from functions that can fail (allocators, init functions, I/O operations) |
| C03 | Integer overflow in size calculations | WARNING | `count * sizeof(x)` without overflow check, especially in allocator paths |
| C04 | Signed/unsigned comparison mismatch | WARNING | Comparing `int` with `size_t` or `uint*_t`, loop counters with unsigned bounds |
| C05 | Buffer access without bounds check | WARNING | Array indexing with unchecked index, especially from external input |
| C06 | Use-after-free patterns | ERROR | Pointer used after being passed to a free function (`kfree`, `pmm_free_page`, etc.) |
| C07 | Uninitialized local variables | WARNING | Local variable read before being assigned, especially pointers and structs |
| C08 | Implicit fallthrough in switch cases | WARNING | Missing `break`/`return`/`__attribute__((fallthrough))` between non-empty switch cases |
| C09 | Infinite loop without escape | INFO | `while(1)` or `for(;;)` without `break`/`return` — flag only if NOT in a halt/idle context |
| C10 | Pointer arithmetic on `void*` | ERROR | Arithmetic on void pointers — undefined behavior in C standard |

### Agent 4: Kernel-Specific Hazards (K01–K10) — Always Active

| ID | Check | Severity | Pattern |
|----|-------|----------|---------|
| K01 | Missing `volatile` on MMIO/hardware register access | ERROR | Pointers to hardware addresses (MMIO regions, device registers) cast without `volatile` qualifier |
| K02 | Magic numbers in hardware code | WARNING | Raw hex/decimal literals in I/O operations — should be named constants (e.g., `#define COM1_PORT 0x3F8`) |
| K03 | Large stack allocations | WARNING | Local arrays > 512 bytes, VLAs (`type arr[n]` where `n` is variable), `alloca()` |
| K04 | Missing `__attribute__((packed))` on hardware structs | WARNING | Structs mapped to hardware/protocol layouts (GDT entries, IDT entries, page table entries, ACPI tables) without `packed` attribute |
| K05 | Hardcoded kernel addresses | WARNING | Raw address literals (e.g., `0xFFFFFFFF80000000`) outside config/constant headers — should reference `KERNEL_VIRT_BASE` etc. |
| K06 | Physical/virtual address confusion | WARNING | Passing a physical address to a function expecting virtual, or vice versa — look for missing `phys_to_virt()`/`virt_to_phys()` calls |
| K07 | Assembly/C calling convention mismatch | ERROR | ASM functions not preserving callee-saved registers (RBX, RBP, R12-R15), wrong argument register usage (RDI, RSI, RDX, RCX, R8, R9 for args) |
| K08 | Missing `cli`/`sti` around critical hardware operations | WARNING | Modifying shared hardware state (PIC, GDT, IDT, page tables) without disabling interrupts |
| K09 | Busy-wait without timeout | INFO | Infinite polling loops on hardware status registers (e.g., serial TX ready) with no timeout or retry limit |
| K10 | Incorrect I/O port access width | WARNING | Using `outb` where `outw`/`outl` is needed, or vice versa, based on device spec |

### Agent 5: Concurrency (L01–L05) — Active from Phase 3+

**Gated**: Skip this agent if no Phase 3+ chunks are marked DONE. Print: "Agent 5 (Concurrency): Deferred until Phase 3 — no threading code yet"

| ID | Check | Severity | Pattern |
|----|-------|----------|---------|
| L01 | Shared mutable state without lock | ERROR | Global/static variables modified from multiple contexts (ISR + main, thread + thread) without spinlock/mutex |
| L02 | Lock ordering violations | WARNING | Two or more locks acquired in different orders across call sites — deadlock risk |
| L03 | Holding lock across blocking operation | ERROR | Spinlock held while calling a function that may sleep/block |
| L04 | Missing interrupt disable in spinlock | ERROR | Spinlock acquired without `cli` — ISR could deadlock on same lock |
| L05 | Non-atomic read-modify-write on shared data | WARNING | `counter++` or `flags |= X` on shared variables without atomic operations or locks |

### Agent 6: Maintainability (M01–M07) — Always Active

| ID | Check | Severity | Pattern |
|----|-------|----------|---------|
| M01 | TODO/FIXME/HACK/XXX tracking | INFO | List all such markers with `file:line` and surrounding context |
| M02 | Functions over 80 lines | WARNING | Function body exceeds 80 lines — suggest splitting |
| M03 | Nesting depth over 4 levels | WARNING | More than 4 levels of `if`/`for`/`while`/`switch` nesting |
| M04 | Excessive global/static mutable variables | WARNING | More than 3 non-const file-scope variables in a single `.c` file |
| M05 | Implementation code in headers | WARNING | Function bodies in `.h` files except `static inline` functions and macros |
| M06 | Dead/unreachable code | INFO | Code after unconditional `return`/`goto`/`break`, `#if 0` blocks, functions defined but never called |
| M07 | Duplicate code blocks | INFO | Near-identical code blocks (>5 lines) appearing in multiple places — suggest extraction |

### Agent 7: Security (X01–X06) — Active from Phase 5+

**Gated**: Skip this agent if no Phase 5+ chunks are marked DONE. Print: "Agent 7 (Security): Deferred until Phase 5 — no syscall/userspace code yet"

| ID | Check | Severity | Pattern |
|----|-------|----------|---------|
| X01 | User pointer used without validation | ERROR | Pointer from syscall argument dereferenced without `copy_from_user`/`copy_to_user` or bounds check |
| X02 | Kernel info leak to userspace | WARNING | Kernel addresses, stack contents, or uninitialized struct padding returned to user — structs should be zeroed before copy-out |
| X03 | Missing permission check | WARNING | Syscall handler performing privileged operation without checking process capabilities/UID |
| X04 | TOCTOU race in path/resource access | WARNING | Check (e.g., permission) and use (e.g., open) on same resource separated by code that could allow concurrent modification |
| X05 | Integer truncation in syscall arguments | WARNING | 64-bit syscall argument cast to 32-bit or smaller without range check |
| X06 | Unbounded copy from user | ERROR | `copy_from_user` or equivalent with user-controlled size and no maximum cap |

---

## Sub-Agent Prompt Template

When launching each sub-agent, use this structure as the Task prompt:

```
You are auditing arc_os kernel source files for [CATEGORY NAME] issues.

## Files to Scan
[LIST OF FILE PATHS]

## Test Files (Relaxed Mode)
Files under tests/ use relaxed rules: boundary violations (A01) and missing prefixes (S04)
are INFO severity instead of ERROR/WARNING.

## Checks
[PASTE THE FULL CHECK TABLE FOR THIS AGENT]

## Instructions
1. Use the Read tool to read each file listed above
2. Apply every check in the table to each file
3. For each finding, record: [CHECK_ID] file_path:line_number: description
4. Group findings by severity: ERRORS first, then WARNINGS, then INFO
5. If a file has zero findings for a check, do not mention it
6. Be precise about line numbers — cite the exact line
7. Return ONLY the findings list, no preamble. If zero findings, return "No findings."
```

---

## Report Format

After all sub-agents return, merge their findings and present:

```
## Audit Report: <scope>

**Files scanned**: <count>
**Agents run**: <active_count>/7 (<list deferred agents if any>)
**Findings**: <error_count> errors, <warning_count> warnings, <info_count> info

### ERRORS (<count>)
- [A01] kernel/mm/pmm.c:12: Includes `kernel/proc/scheduler.h` — mm cannot depend on proc
- [C06] kernel/mm/kmalloc.c:87: `ptr` used at line 92 after `kfree(ptr)` at line 87
- [K01] kernel/drivers/fb_console.c:34: MMIO pointer `fb_addr` missing `volatile`

### WARNINGS (<count>)
- [S04] kernel/mm/pmm.c:25: `alloc_page()` missing subsystem prefix → `pmm_alloc_page()`
- [K02] kernel/arch/x86_64/serial.c:8: Magic number `0x3F8` — define as `COM1_PORT`
- [M02] kernel/boot/limine.c:15: Function `parse_memory_map()` is 112 lines — consider splitting

### INFO (<count>)
- [M01] kernel/mm/vmm.c:45: TODO: "implement large page support"
- [K09] kernel/arch/x86_64/serial.c:30: Busy-wait on LSR without timeout

### Deferred Checks
- Agent 5 (Concurrency): Deferred until Phase 3 — no threading code yet
- Agent 7 (Security): Deferred until Phase 5 — no syscall/userspace code yet

### Top Recommendations
1. <most impactful fix — typically the most common ERROR>
2. <second most impactful>
3. <third most impactful>
```

## Auto-Fix Offer

After presenting the report, offer to auto-fix **safe** changes only:

**Safe auto-fixes** (offer these):
- Tabs → 4 spaces (S08)
- Header guard rename (S02)
- Add doc comment stubs `/* TODO: document */` (S07)
- Trim trailing whitespace (S12)
- Replace magic numbers with named constants in simple cases (K02) — only when the value-to-name mapping is unambiguous (e.g., `0x3F8` → `COM1_PORT`)

**Manual-review items** (flag in report, NEVER auto-fix):
- Abstraction boundary violations (A01) — architectural decision
- Function/type renames (S03, S04, S05) — may break callers
- Missing `volatile` (K01) — requires understanding intent
- Concurrency issues (L01–L05) — requires understanding data flow
- Security issues (X01–X06) — requires understanding trust boundaries
- Use-after-free (C06) — requires understanding ownership semantics

## Edge Cases

- **No source code**: Report "No kernel source files found. Run `/scaffold` first." and stop.
- **Only assembly files**: Run Agents 1, 4, 6 only — skip C-specific checks (header guards, doc comments, type names, safety, boundaries).
- **Test files** (`tests/`): Relaxed mode — boundary violations (A01) and missing prefixes (S04) are INFO, not ERROR/WARNING. All other checks apply normally.
- **Vendored files**: Skip entirely — `vendor/`, `third_party/`, `kernel/include/`, `limine.h`, `stb_*.h`.
- **Single-file scope**: Still run all relevant agents, but each scans only the specified file(s).
- **Phase detection fails**: If `docs/implementation-plan.md` is missing or unparseable, run ALL 7 agents (conservative default).
- **Headers with `#pragma once`**: Flagged as S11 WARNING.
