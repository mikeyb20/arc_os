# Build System Reference

## Overview

CMake-based build system with a freestanding cross-compilation toolchain file. Builds a kernel ELF, creates a bootable ISO with Limine, and supports host-side unit tests.

## Build Commands

```bash
# Configure (first time)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake

# Build kernel
cmake --build build

# Build and create ISO
cmake --build build --target iso

# Run in QEMU
cmake --build build --target run

# Run host-side tests
cmake --build build --target test

# Clean
cmake --build build --target clean
```

## Toolchain File: `toolchain-x86_64.cmake`

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Cross-compiler
set(CMAKE_C_COMPILER x86_64-elf-gcc)
set(CMAKE_ASM_NASM_COMPILER nasm)

# Freestanding flags
set(CMAKE_C_FLAGS_INIT "-ffreestanding -nostdlib -nostdinc -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie -std=c11")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Linker
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -static")

# Don't try to compile test programs (freestanding = no libc)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)
```

## Linker Script: `kernel/arch/x86_64/linker.ld`

Defines kernel memory layout:
- Entry point: `_start`
- Virtual base: `0xFFFFFFFF80000000`
- Sections: `.text`, `.rodata`, `.data`, `.bss`
- Exports: `_kernel_start`, `_kernel_end`, section boundary symbols

## Build Targets

| Target | Description |
|--------|-------------|
| `kernel.elf` | Kernel binary (default) |
| `iso` | Bootable ISO with Limine |
| `run` | Build + launch QEMU |
| `debug` | Build + launch QEMU with GDB |
| `test` | Host-side unit tests |
| `clean` | Remove build artifacts |

## Directory Layout

```
CMakeLists.txt              # Top-level: orchestrates kernel + tests
toolchain-x86_64.cmake     # Cross-compilation toolchain
kernel/
  CMakeLists.txt            # Kernel build rules
  arch/x86_64/
    linker.ld               # Linker script
    CMakeLists.txt           # Arch-specific sources
tests/
  CMakeLists.txt            # Host-side test build rules
tools/
  run.sh                    # QEMU launch script
  debug.sh                  # QEMU + GDB launch script
  make-iso.sh               # ISO creation script
```

## NASM Integration

CMake NASM support via `enable_language(ASM_NASM)`:
```cmake
enable_language(ASM_NASM)
set(CMAKE_ASM_NASM_FLAGS "-f elf64")
```

Assembly files use `.asm` extension and are compiled with `nasm -f elf64`.

## ISO Creation

Using xorriso with Limine:
```bash
# Create ISO directory structure
mkdir -p isodir/boot/limine
cp build/kernel.elf isodir/boot/
cp limine.conf isodir/boot/limine/
cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin isodir/boot/limine/

# Create ISO
xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot boot/limine/limine-uefi-cd.bin \
  -efi-boot-part --efi-boot-image --protective-msdos-label \
  isodir -o build/arc_os.iso

# Install Limine
limine/limine bios-install build/arc_os.iso
```
