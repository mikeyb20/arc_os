# plan-feature

Produce a concrete implementation plan for a new kernel feature or subsystem. Understands the project architecture, layer hierarchy, and init sequence to ensure nothing is forgotten — tests, debug output, build system integration, and init ordering are all covered. This is the first step in the plan → implement → verify lifecycle.

## When to Use

Invoke before implementing any non-trivial feature, new subsystem, or extension to an existing subsystem. Use at the start of a phase chunk, when adding a new driver, or any time the change touches multiple files. The output feeds directly into `/guide-implementation`.

## Scope Determination

- **Named feature**: `/plan-feature PCI enumeration` → plan the PCI subsystem
- **Phase chunk**: `/plan-feature phase 4 chunk 1` → plan the chunk from `overview.md`
- **Extension**: `/plan-feature add sleep queues to scheduler` → plan an extension to `proc/sched`
- **No argument**: Ask the user what feature to plan

---

## Execution Flow

### Step 1: Gather Context (3 Parallel Sub-Agents)

Launch three sub-agents **simultaneously** using the Agent tool to gather all necessary context before designing the plan.

#### Agent A: Architecture Context

```
You are gathering architectural context for planning a new arc_os kernel feature: [FEATURE NAME].

## Instructions
1. Read `overview.md` — find the phase and chunk this feature belongs to, its description,
   design decisions, and dependencies on prior phases
2. Read `CLAUDE.md` — extract naming conventions, abstraction boundary rules, and directory structure
3. Read `.claude/skills/arc-os-architecture/reference/memory-layout.md` and
   `.claude/skills/arc-os-architecture/reference/hal-interface.md` if the feature touches
   memory or hardware

## Output
Return a structured summary:
- **Phase & chunk**: Which phase/chunk this belongs to
- **Layer placement**: Which directory (arch/, mm/, proc/, fs/, drivers/, etc.)
- **Dependencies (downward only)**: Which existing subsystems this feature calls into
- **Consumers (upward)**: Which future subsystems will use this feature
- **Layer hierarchy position**: Where it sits in the layer stack (lib < boot < arch < mm < drivers < proc < fs/ipc/net < security)
- **Key design decisions from overview.md**: Relevant architectural choices already made
```

#### Agent B: Codebase State

```
You are gathering current codebase state for planning a new arc_os kernel feature: [FEATURE NAME].

## Instructions
1. Read `kernel/CMakeLists.txt` — note the KERNEL_C_SOURCES and KERNEL_ASM_SOURCES lists,
   understand where new files would be added
2. Read `kernel/boot/kmain.c` — map the complete init sequence with line numbers:
   serial_init() → bootinfo_init() → gdt_init() → idt_init() → pic_init() →
   pmm_init() → vmm_init() → kmalloc_init() → thread_init() → sched_init() →
   proc_init() → pit_init() → sti
3. Read `tests/test_main.c` — note the extern declarations (lines 11-26), Suite array
   (lines 49-58), and registration pattern
4. Read `tests/CMakeLists.txt` — note the test_runner source list and CTest entries

## Output
Return:
- **Init sequence**: Full ordered list with kmain.c line numbers
- **Correct init insertion point**: Where the new feature's init should go (with justification)
- **Build system pattern**: How to add new .c and .asm files
- **Test registration pattern**: How to add a new test suite (extern decl, Suite entry, CMake entries)
- **Existing APIs**: List public functions from subsystems the feature will depend on
```

#### Agent C: Prior Art

```
You are finding structural templates for planning a new arc_os kernel feature: [FEATURE NAME].

## Instructions
1. Identify 1-2 existing subsystems most similar to [FEATURE NAME] in structure
   (e.g., for a driver: look at PIT or PIC; for a proc feature: look at thread or sched;
   for a mm feature: look at pmm or vmm)
2. For each similar subsystem, read:
   - The header file (.h) — note structure, guard format, doc comments, function signatures
   - The implementation file (.c) — note includes, static vs public functions, init function,
     kprintf debug output
   - The test file (tests/test_*.c) — note header guards, type reproduction, static stubs,
     #include pattern, test structure, suite export

## Output
Return for each template subsystem:
- **Header pattern**: Guard format, types defined, function signatures
- **Impl pattern**: Init function structure, error handling, kprintf format
- **Test pattern**: Stubs needed, header guards used, failure injection approach
- **Key conventions observed**: Anything the new feature should replicate
```

### Step 2: Classify Feature

Based on the gathered context, classify the feature as one of:

| Type | Description | Example |
|------|-------------|---------|
| **New subsystem** | New directory or new module within an existing directory | PCI driver, VFS layer, mutex implementation |
| **Extension** | New functions added to an existing module | Sleep queues added to scheduler, large page support in VMM |
| **Arch-specific** | New code under `arch/x86_64/` with HAL interface | APIC driver, IOAPIC, TSC timer |
| **Cross-cutting** | Touches multiple layers (rare, requires careful planning) | Capability-based security, memory-mapped I/O framework |

### Step 3: Design API

For each new public function:

```
- Function: `subsystem_function_name(params) → return_type`
  - Purpose: one-line description
  - Error conditions: when it returns NULL/error code
  - Callers: who calls this and from where
```

Requirements:
- Every function uses the subsystem prefix (`pmm_`, `vmm_`, `sched_`, `pci_`, etc.)
- Every function that can fail returns a checkable value (pointer → NULL on failure, int → negative on error)
- Every init function is `void subsystem_init(...)` and calls `kprintf("[SUBSYSTEM] initialized\n")`
- Types use `PascalCase` or `snake_case_t` per CLAUDE.md

### Step 4: Plan Test Strategy

Design the test file structure:

- **File**: `tests/test_<name>.c`
- **Header guards to define**: List which kernel headers to guard (to avoid conflicts with system headers)
- **Types to reproduce**: List struct/enum definitions to copy from guarded headers
- **Static stubs needed**: List functions to stub (`kmalloc`, `kfree`, `kprintf`, subsystem-specific)
- **Failure injection**: Which stubs need force-fail capability (e.g., `kmalloc_force_fail`)
- **State reset function**: `reset_<name>_state()` that cleans up between tests
- **Minimum test count**: 8+ tests
- **Test plan table**:

| Test Name | What It Validates |
|-----------|-------------------|
| `test_<name>_init_basic` | Init function sets up initial state correctly |
| `test_<name>_init_idempotent` | Calling init twice doesn't corrupt state |
| ... | ... |
| `test_<name>_<op>_failure` | Operation handles allocation/resource failure gracefully |
| `test_<name>_null_safety` | NULL arguments don't crash |

### Step 5: Output Plan

Present the complete plan in this format:

```
## Implementation Plan: <Feature Name>

### Classification
- **Type**: New subsystem / Extension / Arch-specific / Cross-cutting
- **Layer**: <directory path>
- **Phase/Chunk**: Phase X, Chunk Y.Z

### Dependency Map
- **Depends on (downward)**: <list of subsystems with their header paths>
- **Consumed by (upward)**: <list of future subsystems>

### Files to Create/Modify

| Action | File | Purpose |
|--------|------|---------|
| CREATE | `kernel/<layer>/<name>.h` | Public API header |
| CREATE | `kernel/<layer>/<name>.c` | Implementation |
| CREATE | `kernel/<layer>/<name>.asm` | Assembly stubs (if needed) |
| MODIFY | `kernel/CMakeLists.txt` | Add to KERNEL_C_SOURCES / KERNEL_ASM_SOURCES |
| MODIFY | `kernel/boot/kmain.c` | Add init call + include |
| CREATE | `tests/test_<name>.c` | Host-side unit tests |
| MODIFY | `tests/test_main.c` | Register test suite |
| MODIFY | `tests/CMakeLists.txt` | Add test file + CTest entry |

### API Design
<function signatures with doc comments, per Step 3>

### Init Sequence Placement
- **Insert after**: `<function>()` (kmain.c line <N>)
- **Insert before**: `<function>()` (kmain.c line <N>)
- **Justification**: <why this position — dependency ordering>

### Implementation Order
1. Header file (`<name>.h`) — types, function declarations, doc comments
2. Minimal implementation (`<name>.c`) — includes header, init function with kprintf, stubs for other functions
3. Build system — add to `kernel/CMakeLists.txt`
4. Init call — add to `kernel/boot/kmain.c` at correct position
5. Core implementation — fill in all functions, error handling, K&R style, 4-space indent
6. Tests — test file with stubs, 8+ tests, failure injection, suite export + registration in `test_main.c` and `CMakeLists.txt`
7. Boot verification — cross-compile build, QEMU boot, check serial output for init message
8. Assembly stubs (if needed) — NASM syntax, under 100 lines, add to `KERNEL_ASM_SOURCES`

### Test Plan
<test table from Step 4>

### Required Stubs
<list of functions to stub in the test file, with their signatures>

### Serial Debug Output
- Init: `kprintf("[<NAME>] <subsystem> initialized\n")`
- Key operations: `kprintf("[<NAME>] <operation>: <details>\n")`
- Errors: `kprintf("[<NAME>] ERROR: <description>\n")`

### Risks & Mitigations
- <risk 1>: <mitigation>
- <risk 2>: <mitigation>
```

---

## Edge Cases

- **Feature spans multiple layers**: Plan each layer's component separately with clear interfaces between them. List cross-layer dependencies explicitly.
- **Feature has no init function**: Skip init sequence placement (e.g., utility libraries in `lib/`). Still require build system and test integration.
- **Feature is assembly-heavy**: Still require a C wrapper with the public API. Assembly file under 100 lines; split if larger.
- **Extending an existing subsystem**: No new header — add declarations to existing header. Modify existing .c file. Still require new tests.
- **Feature depends on unimplemented subsystem**: Flag as a risk. Plan the dependency interface but note it needs a stub until the dependency is implemented.
- **No similar prior art exists**: Use the most structurally similar subsystem available, even if functionally different. Note deviations from the template.
- **User provides partial plan**: Fill in missing sections only. Do not override user-specified design decisions.

## Integration with Lifecycle

- **Output feeds into**: `/guide-implementation` — the plan becomes the step tracker
- **Can be verified by**: `/verify-changes` — after implementation, verify-changes checks everything in the plan was actually done
- **References**: Same conventions as `/audit-code` (header guards, function prefixes, layer hierarchy)
