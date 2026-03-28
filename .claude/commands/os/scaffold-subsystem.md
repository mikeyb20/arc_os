# scaffold-subsystem

Generate boilerplate files for a new kernel subsystem with correct arc_os conventions.

## When to Use

Invoke when adding a new subsystem to the kernel (e.g., "scaffold pmm", "scaffold gdt", "scaffold keyboard"). This creates all required files with correct naming, guards, prefixes, and structure — ready for implementation.

## Subsystem → Directory Mapping

| Subsystem examples | Directory | Notes |
|---|---|---|
| pmm, vmm, kmalloc, slab | `kernel/mm/` | Memory management |
| gdt, idt, isr_stubs, pic, apic, context_switch | `kernel/arch/x86_64/` | Architecture-specific (HAL impl) |
| boot_info, limine_parse | `kernel/boot/` | Boot protocol parsing |
| process, thread, scheduler | `kernel/proc/` | Process & thread management |
| vfs, ext2, tmpfs, devfs | `kernel/fs/` | VFS and filesystem drivers |
| serial, ps2_keyboard, vga, framebuffer | `kernel/drivers/` | Device drivers |
| pipe, shm, signal, msgqueue | `kernel/ipc/` | IPC mechanisms |
| tcp, udp, ip, arp, ethernet | `kernel/net/` | Network stack |
| caps, acl, cred | `kernel/security/` | Permissions & capabilities |
| kprintf, string, bitmap, list | `kernel/lib/` | Kernel utility library |

If the subsystem name is ambiguous (e.g., "timer" could be arch or drivers), ask the user which directory it belongs in.

## Name Derivation Rules

Given a subsystem name `<name>` in directory `<dir>`:

- **Function prefix**: `<name>_` (e.g., `pmm_alloc_page()`, `gdt_init()`)
- **Header guard**: `ARCHOS_<DIR>_<NAME>_H` with path segments uppercased (e.g., `ARCHOS_MM_PMM_H`, `ARCHOS_ARCH_X86_64_GDT_H`)
- **Init function**: `<name>_init(void)` — declared in header, defined in `.c`
- **Files created**: `<name>.h`, `<name>.c`, and optionally `<name>.asm`

## File Templates

### Header file: `kernel/<dir>/<name>.h`

```c
#ifndef ARCHOS_<DIR>_<NAME>_H
#define ARCHOS_<DIR>_<NAME>_H

#include <stdint.h>

/* <Name> subsystem — <brief one-line description>.
 *
 * Public API for the <name> subsystem.
 */

/* Initialize the <name> subsystem. */
void <name>_init(void);

#endif /* ARCHOS_<DIR>_<NAME>_H */
```

### Implementation file: `kernel/<dir>/<name>.c`

```c
#include "<name>.h"
#include "lib/kprintf.h"

void <name>_init(void) {
    // TODO: implement <name> initialization
    kprintf("[<NAME>] initialized\n");
}
```

### Assembly stub (for assembly-only subsystems): `kernel/<dir>/<name>.asm`

Only create this for subsystems that need assembly (isr_stubs, context_switch, gdt_flush, etc.).

```asm
; <name>.asm — <brief description>
; Keep under 100 lines. Transition to C as fast as possible.

section .text
bits 64

global <name>_entry

<name>_entry:
    ; TODO: implement
    ret
```

If the subsystem is assembly-only (no C implementation), skip the `.c` file but still create the `.h` for declarations.

### Test file: `tests/test_<name>.c`

```c
#include <stdio.h>
#include <assert.h>

/* Host-side unit tests for <name> subsystem. */

void test_<name>_placeholder(void) {
    // TODO: add real tests
    printf("test_<name>_placeholder: PASS\n");
}

int main(void) {
    test_<name>_placeholder();
    printf("All <name> tests passed.\n");
    return 0;
}
```

## Build System Integration

- If `kernel/CMakeLists.txt` exists, add the new `.c` (and `.asm`) source file to the appropriate source list
- If the build system doesn't exist yet (Phase 0), note as a TODO: `# TODO: Add <name>.c to CMakeLists.txt when build system is ready`
- If `tests/CMakeLists.txt` exists, add `test_<name>.c` as a test target

## Procedure

1. **Determine directory**: Map the subsystem name to a directory using the table above. If ambiguous, ask.
2. **Create directory**: If `kernel/<dir>/` doesn't exist, create it.
3. **Check for conflicts**: If `kernel/<dir>/<name>.h` or `kernel/<dir>/<name>.c` already exists, warn the user and ask before overwriting.
4. **Generate files**: Create `.h`, `.c` (unless assembly-only), `.asm` (if applicable), and `tests/test_<name>.c` using the templates above.
5. **Update build system**: Add sources to CMakeLists.txt if it exists.
6. **Report summary**:
   - List all files created with full paths
   - Show the init function signature: `void <name>_init(void)`
   - Remind: "Call `<name>_init()` from `kmain()` at the appropriate point in the init sequence"
   - If applicable: "Run `/audit <name>` to verify conventions"

## Edge Cases

- **Files already exist**: Warn and ask before overwriting — never silently clobber
- **Directory doesn't exist**: Create it (e.g., `kernel/net/` might not exist in early phases)
- **Ambiguous name**: Ask the user which directory (e.g., "timer" → arch or drivers?)
- **No build system yet**: Skip CMake updates, print TODO reminder
- **Assembly-only subsystem**: Create `.h` and `.asm` but skip `.c`
- **Subsystem with sub-components**: If the user says "scaffold drivers/serial", treat "serial" as the name and "drivers" as the directory
