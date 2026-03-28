# diagnose

Active kernel debugging — classifies symptoms, gathers evidence, traces code paths, cross-references known issues, and produces a diagnosis report with confidence levels.

## When to Use

Invoke when the kernel crashes, hangs, produces wrong output, fails to build, or tests fail. Unlike `/debug-kernel` which is a static GDB reference, this skill actively runs commands, reads code, and investigates.

## Invocation

- `/diagnose` — auto-detect mode: try build, then test, then boot QEMU
- `/diagnose page fault at 0x8` — targeted crash investigation
- `/diagnose hang after PCI` — last-known-good context for hangs
- `/diagnose test_pipe fails` — host test failure investigation

## Symptom Classification

| ID | Category | Observable | Entry Point |
|----|----------|-----------|-------------|
| S1 | CRASH-EXCEPTION | Serial shows `!!! EXCEPTION:` with register dump | Parse registers, resolve RIP |
| S2 | CRASH-TRIPLE | QEMU resets, truncated/no output | QEMU `-d int,cpu_reset`, binary-search init |
| S3 | HANG | Serial stops mid-sequence | Check spinlocks, wait queues, interrupt state |
| S4 | WRONG-OUTPUT | Kernel runs but incorrect behavior | Trace data flow, check format strings |
| S5 | BUILD-FAIL | CMake/GCC returns non-zero | Parse error type, identify root cause |
| S6 | TEST-FAIL | CTest returns non-zero | Run with verbose, compare stubs vs API |
| S7 | BOOT-FAIL | ISO created but no serial output | Verify ISO, check limine.conf, entry point |

## Procedure

### Step 1: Classify Symptom

**If user provided a description**: Map to a category from the table above using keywords:
- "page fault", "exception", "GPF", "CR2" → S1
- "triple fault", "resets", "reboots" → S2
- "hang", "stuck", "no output after", "stops at" → S3
- "wrong", "incorrect", "garbage", "unexpected" → S4
- "build", "compile", "link", "cmake" → S5
- "test fail", "test_" → S6
- "no output", "blank", "won't boot" → S7

**If no description (auto-detect mode)**:
1. Try building: `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake && cmake --build build 2>&1`
   - If build fails → S5
2. Try host tests: `cmake -B build_host && cmake --build build_host && ctest --test-dir build_host --output-on-failure 2>&1`
   - If tests fail → S6
3. Try booting: `timeout 15 qemu-system-x86_64 -cdrom build/arc_os.iso -serial file:/tmp/arc_diag_serial.log -display none -no-reboot -m 128M 2>&1`
   - Read `/tmp/arc_diag_serial.log`
   - If empty → S7
   - If contains `!!! EXCEPTION:` → S1
   - If truncated (last milestone without expected next) → S3
   - If QEMU exited quickly → S2
   - If runs but wrong behavior → S4

### Step 2: Gather Evidence (Launch Parallel Agents)

Launch up to 4 agents simultaneously using the Agent tool, tailored to the symptom class.

#### For S1 (CRASH-EXCEPTION):

**Agent 1 — Serial Output Analysis**:
Parse the exception dump from serial output. Extract:
- Exception type and vector number
- RIP, RSP, CR2 (if page fault)
- All register values
- Error code interpretation

**Agent 2 — RIP Resolution**:
Run `addr2line -e build/kernel.elf -f <RIP>` to get function name and source line. If RIP is in a known region:
- `0xFFFFFFFF80000000`+ → kernel image (resolve with addr2line)
- `0xFFFFFFFFC0000000`+ → heap (likely use-after-free or corruption)
- `0xFFFF800000000000`+ → HHDM (physical memory access)
- `0x0000000000400000`+ → user space (check if kernel was in user context)
- `< 0x1000` → NULL page dereference

**Agent 3 — Code Path Trace**:
Read the function identified by addr2line. Trace the call path that could lead to the crash. Check:
- Pointer dereferences without NULL checks
- Array bounds
- Page table state assumptions
- Stack overflow (16KB thread stacks)

**Agent 4 — Known Issues Cross-Reference**:
Check against known issues:
- K01: VirtIO disk boot hang — zero serial output with `-device virtio-blk-pci`
- K02: sys_exec stale user pointer — kprintf reads user address after CR3 switch (cosmetic)
- K03: fork RBP zeroing crash — CR2=0xfffffffffffffff8 (fixed)

#### For S2 (CRASH-TRIPLE):

**Agent 1**: Boot QEMU with `-d int,cpu_reset -D /tmp/arc_qemu_debug.log`, analyze the log for the last interrupt before reset.

**Agent 2**: Binary-search the init sequence — check which `[TAG]` messages appeared in serial before the crash. The init order is: `[GDT]` → `[IDT]` → `[PIC]` → `[PMM]` → `[VMM]` → `[HEAP]` → `[THREAD]` → `[SCHED]` → `[PCI]` → `[TTY]` → `[KBD]` → `[SYSCALL]` → `[VFS]`.

**Agent 3**: Read `kernel/boot/kmain.c` and the last subsystem's init function to find what could triple fault.

**Agent 4**: Known issues cross-reference (same as S1).

#### For S3 (HANG):

**Agent 1**: Capture serial output, identify the last printed message and which subsystem it belongs to.

**Agent 2**: Read the code at the hang point. Look for:
- Spinlock deadlocks (acquire without release, nested acquires)
- Wait queue starvation (wq_sleep with no matching wq_wake)
- Disabled interrupts (`cli` without `sti`) preventing timer/preemption
- Infinite loops without yield/schedule

**Agent 3**: Check `git log --oneline -10` for recent changes that might have introduced the hang.

**Agent 4**: Known issues cross-reference.

#### For S4 (WRONG-OUTPUT):

**Agent 1**: Capture and diff expected vs actual serial output.

**Agent 2**: Grep for the format string producing wrong output, trace the data flow backward.

**Agent 3**: Check `git diff HEAD~3..HEAD` for recent changes affecting the output path.

**Agent 4**: Known issues cross-reference.

#### For S5 (BUILD-FAIL):

**Agent 1**: Parse the build error output. Classify:
- Undefined reference → missing source in CMakeLists.txt or missing function implementation
- Type mismatch → header/implementation disagreement
- Missing include → header not found or wrong include path
- Syntax error → locate exact file:line

**Agent 2**: Read the failing file and its headers to understand the root cause.

**Agent 3**: Check `git diff` for recent changes that might have caused the break.

#### For S6 (TEST-FAIL):

**Agent 1**: Run `ctest --test-dir build_host --output-on-failure -V` and capture output.

**Agent 2**: Read the failing test file. Compare test stubs against actual kernel API for drift:
- Check that guarded-out kernel types match the actual kernel header types
- Check that stub function signatures match kernel function signatures
- Look for missing stubs for newly added kernel functions

**Agent 3**: If the test was working before, check `git log` for recent kernel changes that could have changed the API.

#### For S7 (BOOT-FAIL):

**Agent 1**: Verify ISO contents: `ls -la build/arc_os.iso`, check `iso_root/` has kernel.elf and limine files.

**Agent 2**: Read `limine.conf` and verify it references the correct kernel path.

**Agent 3**: Check that the kernel entry point is correct: `readelf -h build/kernel.elf | grep Entry`.

### Step 3: Address Region Classification (for S1/S2 crashes)

When a faulting address (RIP or CR2) is available, classify it:

| Region | Address Range | Meaning |
|--------|--------------|---------|
| NULL page | `0x0` - `0xFFF` | NULL pointer dereference |
| User text | `0x400000` - `0x7FFFFFFFFFFF` | User-space address (wrong context or missing mapping) |
| HHDM | `0xFFFF800000000000` - `0xFFFFBFFFFFFFFFFF` | Physical memory via HHDM (check if physical address is valid) |
| Kernel image | `0xFFFFFFFF80000000` - `0xFFFFFFFF9FFFFFFF` | Kernel code/data (resolve with addr2line) |
| Kernel heap | `0xFFFFFFFFC0000000` - `0xFFFFFFFFDFFFFFFF` | kmalloc'd memory (corruption, use-after-free, or overflow) |
| Stack guard | Near stack boundaries | Stack overflow (16KB per thread) |
| Negative offset | `0xFFFFFFFFFFFFFFxx` | Likely bad pointer arithmetic (e.g., base + negative offset) |

### Step 4: Produce Diagnosis Report

```
## Diagnosis Report

**Symptom class**: S<N> — <CATEGORY>
**Observed**: <what was seen>

### Evidence

1. <evidence item from agent 1>
2. <evidence item from agent 2>
3. <evidence item from agent 3>
4. <known issue match or "No known issue match">

### Address Analysis (if applicable)

- **Faulting address (CR2)**: 0x<addr> — <region classification>
- **Instruction pointer (RIP)**: 0x<addr> → <function>:<line> (<file>)

### Root Cause Analysis

**Most likely**: <description> (confidence: HIGH/MEDIUM/LOW)
**Alternative**: <description> (confidence: LOW)

### Suggested Fixes

1. <primary fix with specific file:line references>
2. <alternative approach>

### Verification Steps

1. <how to verify the fix worked>
2. <regression test to add>

### GDB Commands (if applicable)

```
<specific GDB commands to further investigate>
```
```

### Step 5: Offer to Fix

If confidence is HIGH and the fix is straightforward, offer to implement it. If confidence is MEDIUM or LOW, present options and ask the user.

---

## Known Issues Database

These are known issues tracked in project memory. Cross-reference during every diagnosis:

| ID | Symptom | Description | Status |
|----|---------|-------------|--------|
| K01 | S3/S7 | VirtIO disk causes boot hang (0 serial output) when QEMU uses `-device virtio-blk-pci` | Active — works without VirtIO |
| K02 | S4 | sys_exec kprintf reads stale user pointer after address space switch | Active — cosmetic only |
| K03 | S1 | fork RBP zeroing causes crash at CR2=0xfffffffffffffff8 | Fixed |

---

## Reference: Serial Output Milestones

The kernel prints these markers during boot (in order). Use to identify where a hang or crash occurs:

```
[GDT] ...
[IDT] ...
[PIC] ...
[PIT] ...
[PMM] ...
[VMM] ...
[HEAP] ...
[THREAD] ...
[SCHED] ...
[PCI] ...
[VFS] ...
[TTY] ...
[KBD] ...
[SYSCALL] ...
```

After init, the shell prints `arc_os> ` prompt.

---

## Reference: Exception Register Dump Format

The kernel's exception handler (`kernel/arch/x86_64/isr.c:50-76`) prints:
```
!!! EXCEPTION: <name> (vector <N>, error=0x<code>)
  RIP = 0x<addr>  RSP = 0x<addr>
  RAX = ...  RBX = ...  RCX = ...  RDX = ...
  RSI = ...  RDI = ...  RBP = ...
  R8  = ...  R9  = ...  R10 = ...  R11 = ...
  R12 = ...  R13 = ...  R14 = ...  R15 = ...
  CS  = ...  SS  = ...  RFLAGS = ...
  CR2 = 0x<addr> (faulting address)    [page fault only]
!!! System halted.
```

---

## Difference from `/debug-kernel`

| Aspect | `/debug-kernel` | `/diagnose` |
|--------|----------------|-------------|
| Mode | Passive reference | Active investigation |
| Output | GDB commands, cheat sheet | Diagnosis report with root cause |
| Runs commands | No | Yes (builds, boots QEMU, runs addr2line) |
| Reads code | No | Yes (traces call paths, reads faulting functions) |
| Known issues | Not referenced | Cross-referenced automatically |
| Confidence | N/A | HIGH/MEDIUM/LOW per finding |
| Fix offered | No | Yes, if confidence is HIGH |
