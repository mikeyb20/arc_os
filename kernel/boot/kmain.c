#include "arch/x86_64/serial.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/pit.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "boot/bootinfo.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
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

    /* Initialize GDT with TSS */
    gdt_init();

    /* Initialize IDT with ISR stubs */
    idt_init();

    /* Initialize PIC — remap IRQs to vectors 32-47 */
    pic_init();

    /* Initialize physical memory manager */
    pmm_init(info);

    /* Initialize virtual memory manager — creates kernel page tables */
    vmm_init(info);

    /* Initialize kernel heap allocator */
    kmalloc_init();

    /* Heap self-test: 1000 alloc/free cycles */
    kprintf("[BOOT] Running heap self-test...\n");
    for (int i = 0; i < 1000; i++) {
        size_t sz = 16 + (uint32_t)(i * 37) % 512;  /* Varying sizes 16-527 */
        void *p = kmalloc(sz, GFP_ZERO);
        if (p == NULL) {
            kprintf("[BOOT] FAIL: kmalloc returned NULL at iteration %d\n", i);
            for (;;) __asm__ volatile ("cli; hlt");
        }
        /* Verify zero-fill */
        uint8_t *bytes = (uint8_t *)p;
        for (size_t j = 0; j < sz; j++) {
            if (bytes[j] != 0) {
                kprintf("[BOOT] FAIL: GFP_ZERO not zeroed at iteration %d\n", i);
                for (;;) __asm__ volatile ("cli; hlt");
            }
        }
        /* Write pattern, then free */
        memset(p, 0xAB, sz);
        kfree(p);
    }
    kprintf("[BOOT] Heap self-test passed (1000 alloc/free cycles)\n");
    kmalloc_dump_stats();

    /* Initialize PIT timer at 100 Hz */
    pit_init(100);

    /* Enable interrupts */
    kprintf("[BOOT] Enabling interrupts...\n");
    __asm__ volatile ("sti");

    kprintf("[BOOT] arc_os Phase 1+2 complete. Kernel ready.\n");

    /* Halt loop — HLT wakes on interrupt, then halts again */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
