#ifndef ARCHOS_BOOT_BOOTINFO_H
#define ARCHOS_BOOT_BOOTINFO_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum memory map entries we can store (no heap yet). */
#define BOOTINFO_MAX_MEMMAP_ENTRIES 64

/* Memory map region types (bootloader-agnostic). */
#define MEMMAP_USABLE              0
#define MEMMAP_RESERVED            1
#define MEMMAP_ACPI_RECLAIMABLE    2
#define MEMMAP_ACPI_NVS            3
#define MEMMAP_BAD_MEMORY          4
#define MEMMAP_BOOTLOADER_RECLAIM  5
#define MEMMAP_KERNEL_AND_MODULES  6
#define MEMMAP_FRAMEBUFFER         7

/* A single memory map entry. */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} MemoryMapEntry;

/* Framebuffer descriptor. */
typedef struct {
    void    *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
} Framebuffer;

/* Boot module (loaded by bootloader alongside kernel) */
typedef struct {
    void    *address;
    uint64_t size;
    char     path[128];
} BootModule;

#define BOOTINFO_MAX_MODULES 8

/* Bootloader-agnostic boot information. */
typedef struct {
    MemoryMapEntry memory_map[BOOTINFO_MAX_MEMMAP_ENTRIES];
    uint64_t       memory_map_count;
    Framebuffer    framebuffer;
    bool           fb_present;
    uint64_t       acpi_rsdp;
    uint64_t       kernel_phys_base;
    uint64_t       kernel_virt_base;
    uint64_t       hhdm_offset;
    BootModule     modules[BOOTINFO_MAX_MODULES];
    uint64_t       module_count;
} BootInfo;

/* Parse bootloader data into a BootInfo struct.
 * Returns a pointer to the global BootInfo, or NULL if critical data
 * (memory map, HHDM) is missing. */
const BootInfo *bootinfo_init(void);

#endif /* ARCHOS_BOOT_BOOTINFO_H */
