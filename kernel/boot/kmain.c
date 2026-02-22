#include "arch/x86_64/serial.h"
#include "boot/bootinfo.h"
#include "lib/kprintf.h"
#include <stddef.h>
#include <stdint.h>

static const char *memmap_type_name(uint32_t type) {
    switch (type) {
    case MEMMAP_USABLE:             return "Usable";
    case MEMMAP_RESERVED:           return "Reserved";
    case MEMMAP_ACPI_RECLAIMABLE:   return "ACPI Reclaimable";
    case MEMMAP_ACPI_NVS:           return "ACPI NVS";
    case MEMMAP_BAD_MEMORY:         return "Bad Memory";
    case MEMMAP_BOOTLOADER_RECLAIM: return "Bootloader Reclaimable";
    case MEMMAP_KERNEL_AND_MODULES: return "Kernel/Modules";
    case MEMMAP_FRAMEBUFFER:        return "Framebuffer";
    default:                        return "Unknown";
    }
}

void kmain(void) {
    serial_init();
    serial_puts("[BOOT] arc_os kernel booting...\n");

    const BootInfo *info = bootinfo_init();
    if (info == NULL) {
        serial_puts("[BOOT] FATAL: failed to parse boot info\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    kprintf("[BOOT] HHDM offset: 0x%lx\n", info->hhdm_offset);
    kprintf("[BOOT] Kernel phys base: 0x%lx\n", info->kernel_phys_base);
    kprintf("[BOOT] Kernel virt base: 0x%lx\n", info->kernel_virt_base);

    if (info->fb_present) {
        kprintf("[BOOT] Framebuffer: %lux%lu bpp=%u pitch=%lu addr=%p\n",
                info->framebuffer.width,
                info->framebuffer.height,
                (uint32_t)info->framebuffer.bpp,
                info->framebuffer.pitch,
                info->framebuffer.address);
    } else {
        kprintf("[BOOT] Framebuffer: not available\n");
    }

    kprintf("[BOOT] Memory map (%lu entries):\n", info->memory_map_count);
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const MemoryMapEntry *e = &info->memory_map[i];
        kprintf("  [%lu] 0x%lx - 0x%lx (%lu KB) %s\n",
                i,
                e->base,
                e->base + e->length,
                e->length / 1024,
                memmap_type_name(e->type));
    }

    if (info->acpi_rsdp != 0) {
        kprintf("[BOOT] ACPI RSDP at phys 0x%lx\n", info->acpi_rsdp);
    } else {
        kprintf("[BOOT] ACPI RSDP: not available\n");
    }

    kprintf("[BOOT] Boot info parsed successfully.\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
