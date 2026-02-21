# Boot Protocol Reference

## Overview

arc_os uses the Limine bootloader initially. The kernel consumes a `BootInfo` struct — it never knows or cares which bootloader populated it.

## BootInfo Struct

```c
typedef struct {
    /* Memory map */
    MemoryMapEntry *memory_map;
    uint64_t memory_map_count;

    /* Framebuffer */
    Framebuffer *framebuffer;

    /* ACPI */
    void *acpi_rsdp;           /* Pointer to ACPI RSDP table */

    /* Kernel location */
    uintptr_t kernel_phys_base;
    uintptr_t kernel_virt_base;
    uint64_t kernel_size;

    /* Initrd / modules */
    void *initrd_base;
    uint64_t initrd_size;

    /* Command line */
    const char *cmdline;

    /* Higher-half direct map base */
    uintptr_t hhdm_offset;

    /* SMP info (Phase 12) */
    SmpInfo *smp_info;
    uint64_t cpu_count;
} BootInfo;

typedef struct {
    uintptr_t base;
    uint64_t length;
    uint32_t type;             /* USABLE, RESERVED, ACPI_RECLAIMABLE, etc. */
} MemoryMapEntry;

typedef struct {
    void *address;             /* Framebuffer base address (virtual) */
    uint64_t width;
    uint64_t height;
    uint64_t pitch;            /* Bytes per row */
    uint16_t bpp;              /* Bits per pixel */
    /* Pixel format info */
    uint8_t red_mask_size, red_mask_shift;
    uint8_t green_mask_size, green_mask_shift;
    uint8_t blue_mask_size, blue_mask_shift;
} Framebuffer;
```

## Memory Map Entry Types

```c
#define MEMMAP_USABLE                 0
#define MEMMAP_RESERVED               1
#define MEMMAP_ACPI_RECLAIMABLE       2
#define MEMMAP_ACPI_NVS               3
#define MEMMAP_BAD_MEMORY             4
#define MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define MEMMAP_KERNEL_AND_MODULES     6
#define MEMMAP_FRAMEBUFFER            7
```

## Limine-Specific Setup

### limine.conf (boot configuration)
```
timeout: 3

/arc_os
    protocol: limine
    kernel_path: boot():/kernel.elf
    # module_path: boot():/initrd.img
```

### Boot Flow
1. BIOS/UEFI loads Limine from ISO
2. Limine loads `kernel.elf` to specified address
3. Limine sets up higher-half mappings, long mode, GDT
4. Limine jumps to kernel entry point
5. Kernel entry stub parses Limine responses into `BootInfo`
6. Kernel calls `kmain(BootInfo *info)`

### Abstraction Boundary
- `kernel/boot/limine.c` — Limine-specific code that reads Limine protocol structs
- `kernel/boot/bootinfo.h` — Generic `BootInfo` definition (bootloader-agnostic)
- Only `limine.c` knows about Limine. Everything else reads `BootInfo`.
- To switch bootloaders: write a new `kernel/boot/newloader.c` that populates the same `BootInfo`
