# Boot Protocol Reference

The definitions live in the code; this page only points at them (ADR-0020: docs don't restate code).

The kernel boots with Limine and reads everything the bootloader found from one bootloader-neutral
`BootInfo` struct. Code outside `kernel/boot/` never sees Limine types.

| What | Where it is defined |
|---|---|
| `BootInfo` (memory map, framebuffer, RSDP, HHDM offset, modules, kernel command line) and its limits (`BOOTINFO_MAX_*`, `BOOTINFO_CMDLINE_MAX`) | `kernel/boot/bootinfo.h` |
| Memory map entry types (`MEMMAP_*`) | `kernel/boot/bootinfo.h` |
| Limine requests (the base revision, memmap, modules, SMP, ...) | `kernel/boot/limine_requests.c` |
| Limine responses → `BootInfo` (`bootinfo_init()`) | `kernel/boot/limine.c` |
| Command-line options (`arc.test=`, `arc.mem=`, ...) | `kernel/lib/cmdline.h` and the code that reads each option |
| Boot menu and modules on the ISO | `limine.conf`, `tools/make-iso.sh` (the QEMU harness adds test modules through `tools/qemu-test.py`) |
| Pinned Limine version | `tools/toolchain.lock` (fetched by `tools/dev-setup.sh limine`) |

Boot flow: firmware → Limine → `kernel.elf` entry (long mode, higher half, HHDM already mapped) →
`kmain()` in `kernel/boot/kmain.c` calls `bootinfo_init()` (`kernel/boot/limine.c`) to fill `BootInfo`
from the Limine responses, then brings up the subsystems.

To support another bootloader, write a new `kernel/boot/<loader>.c` that fills the same `BootInfo`.
Known exception: `kernel/arch/x86_64/smp.c` reads Limine's SMP response directly.
