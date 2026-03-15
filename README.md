# arc_os

A custom x86_64 operating system built from scratch. Monolithic kernel written in C (C11) with NASM assembly stubs, targeting POSIX compatibility. Boots via the Limine bootloader, runs in QEMU, and aims to eventually support a shell, coreutils, and real hardware.

## Current Status

The kernel boots in QEMU and has the following working subsystems:

- **Boot**: Limine bootloader, serial output, kprintf, bootloader-agnostic BootInfo
- **Hardware**: GDT/TSS, IDT with all 256 vectors, PIC (remapped), PIT timer at 100Hz, PS/2 keyboard
- **Memory**: PMM (bitmap allocator), VMM (4-level paging, own page tables), kmalloc (free-list heap)
- **Processes**: Per-process address spaces, ELF64 loader, fork/exec/wait, user pointer validation
- **Threading**: Thread creation, context switch, round-robin preemptive scheduler, spinlocks
- **Syscalls**: SYSCALL/SYSRET entry, 21 syscalls (exit, write, getpid, open, read, close, brk, lseek, stat, mkdir, readdir, unlink, dup2, fork, exec, wait, pipe, signal, kill, sigreturn, and more)
- **Drivers**: PCI bus enumeration, VirtIO common infrastructure, VirtIO-blk (polling read), PS/2 keyboard, TTY subsystem
- **Filesystem**: VFS layer with ramfs (in-memory create/read/write/unlink), file syscalls
- **IPC**: Unix pipes (`cmd1 | cmd2`), POSIX signals (signal/kill/sigreturn, SIGINT/SIGCHLD/SIGPIPE, Ctrl+C)
- **Shell**: Interactive shell with 14 builtins (ls, cat, mkdir, rm, touch, write, stat, run, echo, pid, uname, help, clear, exit), pipe support
- **User Binaries**: init, hello, echo, shell — loaded as Limine boot modules
- **Tests**: 27 host-side test suites, 374 tests — all passing

See `overview.md` for the full 13-phase development roadmap.

## Quick Start

### Prerequisites

- `x86_64-elf-gcc` cross-compiler (see `tools/build-cross-compiler.sh`)
- `nasm`, `qemu-system-x86_64`, `xorriso`, `cmake`
- Install system deps: `tools/setup-deps.sh`

### Build

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake
cmake --build build
```

### Run

```bash
cmake --build build --target run
```

Or manually:
```bash
qemu-system-x86_64 -cdrom build/arc_os.iso -serial stdio -m 256M -no-reboot
```

### Test

```bash
cmake -B build_host
cmake --build build_host
ctest --test-dir build_host
```

### Debug

```bash
# Terminal 1
cmake --build build --target debug
# Terminal 2
gdb build/kernel.elf -ex "target remote :1234" -ex "break kmain" -ex "continue"
```

## Architecture

```
kernel/
├── arch/x86_64/   # GDT, IDT, PIC, PIT, paging, context switch, ISR stubs, syscall entry
├── boot/          # Limine integration, BootInfo abstraction, kprintf
├── mm/            # PMM (bitmap), VMM (4-level paging), kmalloc (free-list)
├── proc/          # Threads, processes, scheduler, ELF loader, fork/exec/wait, signals
├── fs/            # VFS layer, ramfs, pipes
├── drivers/       # PCI, VirtIO, VirtIO-blk, PS/2 keyboard, TTY
├── include/       # Freestanding C headers + limine.h
└── lib/           # memcpy/memset, string functions, kprintf
userland/
├── init/          # Init process (fork/exec orchestrator)
├── shell/         # Interactive shell (14 builtins)
├── echo/          # Echo binary (interactive stdin echo)
└── hello/         # Hello world binary (exec target)
```

- Kernel mapped at `0xFFFFFFFF80000000`, HHDM at `0xFFFF800000000000`
- Heap at `0xFFFFFFFFC0000000`
- User processes at `0x400000`, user stack at `0x7FFFFFFFE000`
- All arch-specific code is under `kernel/arch/x86_64/`
- Bootloader abstracted behind `BootInfo` struct

## License

MIT
