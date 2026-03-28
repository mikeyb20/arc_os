# /plan — Create Development Plan

## Instructions

You are creating a development plan. Your goal is to produce a plan file that
eliminates all ambiguity before implementation begins. Every architectural
decision must be made explicitly in this plan — the implementation agent should
never need to guess.

## Step 1: Assess Tier

Determine the scope by analyzing the user's description:
- **Tier 1 (Standard):** 3-10 files, single concern, one workstream
- **Tier 2 (Complex):** 10+ files, multiple concerns, cross-cutting, or
  requires parallel workstreams

If the task is 1-2 files with an obvious fix, tell the user this doesn't
need a plan — just implement it directly.

## Step 2: Gather Context (3 Parallel Sub-Agents)

Launch three sub-agents **simultaneously** using the Agent tool.

### Agent A: Architecture Context

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

### Agent B: Codebase State

```
You are gathering current codebase state for planning a new arc_os kernel feature: [FEATURE NAME].

## Instructions
1. Read `kernel/CMakeLists.txt` — note the KERNEL_C_SOURCES and KERNEL_ASM_SOURCES lists,
   understand where new files would be added
2. Read `kernel/boot/kmain.c` — map the complete init sequence with line numbers
3. Read `tests/test_main.c` — note the extern declarations, Suite array, and registration pattern
4. Read `tests/CMakeLists.txt` — note the test_runner source list and CTest entries

## Output
Return:
- **Init sequence**: Full ordered list with kmain.c line numbers
- **Correct init insertion point**: Where the new feature's init should go (with justification)
- **Build system pattern**: How to add new .c and .asm files
- **Test registration pattern**: How to add a new test suite (extern decl, Suite entry, CMake entries)
- **Existing APIs**: List public functions from subsystems the feature will depend on
```

### Agent C: Prior Art

```
You are finding structural templates for planning a new arc_os kernel feature: [FEATURE NAME].

## Instructions
1. Identify 1-2 existing subsystems most similar to [FEATURE NAME] in structure
2. For each similar subsystem, read:
   - The header file (.h) — note structure, guard format, doc comments, function signatures
   - The implementation file (.c) — note includes, static vs public functions, init function
   - The test file (tests/test_*.c) — note header guards, stubs, test structure, suite export

## Output
Return for each template subsystem:
- **Header pattern**: Guard format, types defined, function signatures
- **Impl pattern**: Init function structure, error handling, kprintf format
- **Test pattern**: Stubs needed, header guards used, failure injection approach
- **Key conventions observed**: Anything the new feature should replicate
```

## Step 3: Classify Feature

Based on the gathered context, classify the feature:

| Type | Description | Example |
|------|-------------|---------|
| **New subsystem** | New directory or new module within an existing directory | PCI driver, VFS layer, mutex implementation |
| **Extension** | New functions added to an existing module | Sleep queues added to scheduler, large page support in VMM |
| **Arch-specific** | New code under `arch/x86_64/` with HAL interface | APIC driver, IOAPIC, TSC timer |
| **Cross-cutting** | Touches multiple layers (rare, requires careful planning) | Capability-based security, memory-mapped I/O framework |

## Step 4: Write Plan

Create `docs/plans/<feature-name>.md` using the appropriate template.

### Tier 1 Template:

```
# <Feature Name>

## What
<One sentence describing the deliverable>

## Why
<What problem this solves — prevents scope creep later>

## Boundaries
<What this change does NOT touch — be explicit>

## Files to Create/Modify
- <path>: <what changes and why>

## Interfaces
<Function signatures, types, or API contracts introduced or changed>

## Done Criteria
- [ ] <Specific, testable condition>
- [ ] All existing tests pass
- [ ] New tests cover <specific scenarios>

## Test Approach
<What to test, which framework, TDD or post-implementation>
```

### Tier 2 Template (adds to Tier 1):

```
## Architecture Impact
<How this changes the system structure, new dependencies, affected modules>

## Workstream Breakdown

### Shared Interfaces (build first)
- <type/interface>: <file path> — <description>

### Workstream A: <name>
- **Files:** <exact list of files this workstream owns>
- **Depends on:** <interfaces from shared or other workstreams>
- **Exposes:** <interfaces other workstreams consume>
- **Estimated complexity:** <low/medium/high>

## Dependency Graph
<Which workstreams depend on which, in what direction>

## Conflict Risk Assessment
- <File/module> touched by workstreams <A> and <B> — resolution: <who owns it>
If any HIGH conflict risks exist, redesign the split. Do NOT approve with unresolved HIGH risks.

## Merge Order
1. Shared interfaces
2. <Producer workstream> — rationale: <why>
3. <Consumer workstream> — rationale: <depends on producer>
```

### arc_os API Design Conventions

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

### Init Sequence Placement

- **Insert after**: `<function>()` (kmain.c line <N>)
- **Insert before**: `<function>()` (kmain.c line <N>)
- **Justification**: <why this position — dependency ordering>

### Implementation Order

1. Header file (`<name>.h`) — types, function declarations, doc comments
2. Minimal implementation (`<name>.c`) — includes header, init function with kprintf, stubs
3. Build system — add to `kernel/CMakeLists.txt`
4. Init call — add to `kernel/boot/kmain.c` at correct position
5. Core implementation — fill in all functions, error handling, K&R style, 4-space indent
6. Tests — test file with stubs, 8+ tests, failure injection, suite export + registration
7. Boot verification — cross-compile build, QEMU boot, check serial output for init message
8. Assembly stubs (if needed) — NASM syntax, under 100 lines

## Step 5: Parallelism Validation (Tier 2 only)

For Tier 2 plans:

### 5a. Build File Dependency Graph
For every file: classify as CREATE or MODIFY, map imports/dependencies.

### 5b. Verify Independence
- No two workstreams WRITE to the same file
- Shared type files owned by ONE workstream, read-only for others
- If a file MUST be written by two workstreams, serialize those tasks

### 5c. Define Contracts
For each workstream boundary: exact signatures, direction (producer/consumer), build order.

## Step 6: Self-Review

Before presenting the plan, verify:
- [ ] Every file that will be touched is listed explicitly
- [ ] No interface is consumed by one workstream but not produced by any
- [ ] Done criteria are testable (not vague like "works correctly")
- [ ] Boundaries are stated — what is explicitly OUT of scope
- [ ] For Tier 2: workstream file lists don't overlap
- [ ] For Tier 2: merge order respects dependency direction

## Step 7: Present for Review

Present the plan to the user. Explicitly call out:
- Decisions you made that the user should confirm
- Risks or uncertainties you identified
- Alternative approaches you considered and why you chose this one

Do NOT begin implementation. Wait for user approval.

---

## Edge Cases

- **Feature spans multiple layers**: Plan each layer's component separately with clear interfaces between them.
- **Feature has no init function**: Skip init sequence placement (e.g., utility libraries in `lib/`).
- **Feature is assembly-heavy**: Still require a C wrapper with the public API. Assembly file under 100 lines.
- **Extending an existing subsystem**: No new header — add declarations to existing header. Still require new tests.
- **User provides partial plan**: Fill in missing sections only. Do not override user-specified design decisions.

## Integration with Lifecycle

- **Output feeds into**: `/build/implement` — the plan becomes the step tracker
- **Can be verified by**: `/validate/review` and `/validate/done-check` — verify everything in the plan was done
- **References**: Same conventions as `/validate/sweep` (header guards, function prefixes, layer hierarchy)
