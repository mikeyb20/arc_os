# implement-phase

Disciplined workflow for implementing each OS development phase.

## Workflow

When implementing a phase (or sub-phase), follow these steps in order:

### 1. Read the Phase Specification
- Read `overview.md` and find the relevant phase section
- Understand what needs to be built, what's being leveraged, and what the milestone is
- Identify the abstraction boundaries this phase must respect

### 2. Check Current State
- What code already exists? (`find kernel/ -name '*.c' -o -name '*.h' -o -name '*.asm'`)
- What phases are already implemented?
- Are there any dependencies from earlier phases that are missing?

### 3. Plan the Implementation
- Break the phase into small, testable chunks
- Identify which files need to be created/modified
- Define the public API (header) before writing the implementation
- Plan how to test each chunk (serial output, GDB inspection, host-side tests)

### 4. Implement with Abstraction Boundaries
- Write the header (public interface) first
- Implement behind the interface
- Keep arch-specific code in `kernel/arch/<arch>/`
- Use subsystem prefixes on all functions: `pmm_`, `vmm_`, `hal_`, `vfs_`, `sched_`
- Follow existing naming conventions (see CLAUDE.md)

### 5. Test in QEMU
- Build: `cmake --build build`
- Boot: `./tools/run.sh`
- Check serial output for expected behavior
- Use GDB if debugging is needed: `./tools/run.sh -s -S`

### 6. Update Phase Status
- Add a status note to `overview.md` or `docs/` indicating what's complete
- Document any deviations from the original plan
- Note any TODOs or known limitations

## Phase Implementation Order

Follow the "Suggested Order of Attack" in overview.md:
1. Phase 0 → Toolchain, build system, QEMU boots
2. Phase 1.1-1.2 → Boot via Limine, "Hello" on serial
3. Phase 1.3-1.5 → Interrupts, timer, keyboard
4. Phase 2.1-2.3 → PMM, paging, kmalloc
5. Phase 3.1-3.3 → Kernel threads, scheduler
6. ... (see overview.md for full sequence)

## Common Patterns

### Adding a New Subsystem
1. Create `kernel/<subsystem>/<subsystem>.h` (public API)
2. Create `kernel/<subsystem>/<subsystem>.c` (implementation)
3. Add to `kernel/CMakeLists.txt`
4. Call `<subsystem>_init()` from `kmain()` at the appropriate point
5. Add test in `tests/test_<subsystem>.c`

### Adding an Arch-Specific Feature
1. Add function declaration to `kernel/arch/hal.h`
2. Implement in `kernel/arch/x86_64/<feature>.c` or `.asm`
3. Never call arch-specific functions directly from generic kernel code
