#ifndef ARCHOS_BOOT_BOOTINFO_H
#define ARCHOS_BOOT_BOOTINFO_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum memory map entries we can store (no heap yet). */
#define BOOTINFO_MAX_MEMMAP_ENTRIES 256  /* q35 + UEFI maps exceed 64 entries */

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

#define BOOTINFO_MAX_MODULES 32

/* Kernel command line buffer size, including the NUL. */
#define BOOTINFO_CMDLINE_MAX 256

/* CPUs the bootloader found and can start (BootInfo.cpus) */
#define BOOTINFO_MAX_CPUS 256

/* How the machine booted (BootInfo.firmware) */
#define BOOT_FIRMWARE_UNKNOWN 0
#define BOOT_FIRMWARE_BIOS    1
#define BOOT_FIRMWARE_UEFI32  2
#define BOOT_FIRMWARE_UEFI64  3

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
    char           cmdline[BOOTINFO_CMDLINE_MAX];  /* Kernel command line ("" if none) */
    uint32_t cpu_apic_ids[BOOTINFO_MAX_CPUS];  /* every CPU, BSP included */
    uint32_t cpu_count;                        /* 0: no SMP information */
    uint32_t bsp_apic_id;
    uint64_t smbios_entry32;   /* physical address of the SMBIOS 2.x entry point, 0 if none */
    uint64_t smbios_entry64;   /* physical address of the SMBIOS 3.x entry point, 0 if none */
    uint32_t firmware;         /* BOOT_FIRMWARE_* */
    /* Where Limine loaded the kernel from (ADR-0018: arc_os mounts its
     * partitions from that disk only): the disk's GPT GUID and the
     * partition's, in GPT byte order (first three fields little-endian).
     * boot_gpt false (an ISO, PXE, an MBR disk): both all zeros. */
    uint8_t boot_disk_guid[16];
    uint8_t boot_part_guid[16];
    bool boot_gpt;
} BootInfo;

/* Parse bootloader data into a BootInfo struct.
 * Returns a pointer to the global BootInfo, or NULL if critical data
 * (memory map, HHDM) is missing. */
const BootInfo *bootinfo_init(void);

/* Entry point of a started CPU; arg is the value given to boot_start_cpu. It
 * runs on a small bootloader stack with interrupts off and never returns. */
typedef void (*boot_cpu_entry_t)(uint64_t arg);

/* Start BootInfo CPU number i (not the BSP) at entry(arg). Every call must pass
 * the same entry. Returns 0, or -1 if i is out of range or the BSP. */
int boot_start_cpu(uint32_t i, boot_cpu_entry_t entry, uint64_t arg);

#endif /* ARCHOS_BOOT_BOOTINFO_H */
