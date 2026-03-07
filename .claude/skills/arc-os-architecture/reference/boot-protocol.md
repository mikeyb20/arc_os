# Boot Protocol Reference

## Overview

arc_os uses the Limine bootloader initially. The kernel consumes a `BootInfo` struct — it never knows or cares which bootloader populated it.

## BootInfo Struct

Defined in `kernel/boot/bootinfo.h`. Key design: uses fixed-size arrays (no heap available at parse time).

```c
#define BOOTINFO_MAX_MEMMAP_ENTRIES 64
#define BOOTINFO_MAX_MODULES 8

typedef struct {
    MemoryMapEntry memory_map[BOOTINFO_MAX_MEMMAP_ENTRIES];
    uint64_t       memory_map_count;
    Framebuffer    framebuffer;
    bool           fb_present;          /* Whether a framebuffer was provided */
    uint64_t       acpi_rsdp;           /* Physical address of ACPI RSDP */
    uint64_t       kernel_phys_base;
    uint64_t       kernel_virt_base;
    uint64_t       hhdm_offset;
    BootModule     modules[BOOTINFO_MAX_MODULES];
    uint64_t       module_count;
} BootInfo;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;             /* MEMMAP_USABLE, MEMMAP_RESERVED, etc. */
} MemoryMapEntry;

typedef struct {
    void    *address;          /* Framebuffer base address (virtual) */
    uint64_t width;
    uint64_t height;
    uint64_t pitch;            /* Bytes per row */
    uint16_t bpp;              /* Bits per pixel */
    uint8_t  red_mask_size, red_mask_shift;
    uint8_t  green_mask_size, green_mask_shift;
    uint8_t  blue_mask_size, blue_mask_shift;
} Framebuffer;

typedef struct {
    void    *address;
    uint64_t size;
    char     path[128];
} BootModule;
```

## Memory Map Entry Types

```c
#define MEMMAP_USABLE              0
#define MEMMAP_RESERVED            1
#define MEMMAP_ACPI_RECLAIMABLE    2
#define MEMMAP_ACPI_NVS            3
#define MEMMAP_BAD_MEMORY          4
#define MEMMAP_BOOTLOADER_RECLAIM  5
#define MEMMAP_KERNEL_AND_MODULES  6
#define MEMMAP_FRAMEBUFFER         7
```

## Limine-Specific Setup

### limine.conf (boot configuration)
```
timeout: 0

/arc_os
    protocol: limine
    kernel_path: boot():/boot/kernel.elf
    module_path: boot():/boot/init
```

### Boot Flow
1. BIOS/UEFI loads Limine from ISO
2. Limine loads `kernel.elf` to specified address
3. Limine sets up higher-half mappings, long mode, GDT
4. Limine jumps to kernel entry point
5. Kernel entry stub parses Limine responses into `BootInfo`
6. Kernel calls `kmain()` (BootInfo is a global, not a parameter)

### Abstraction Boundary
- `kernel/boot/limine_requests.c` — Embeds all Limine request structs (the ONLY file that includes `<limine.h>`)
- `kernel/boot/limine.c` — Reads Limine responses and populates the global `BootInfo`
- `kernel/boot/bootinfo.h` — Generic `BootInfo` definition (bootloader-agnostic)
- Only `limine_requests.c` and `limine.c` know about Limine. Everything else reads `BootInfo`.
- To switch bootloaders: write a new `kernel/boot/newloader.c` that populates the same `BootInfo`
