#include "arch/x86_64/serial.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/pit.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "boot/bootinfo.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/process.h"
#include "drivers/pci.h"
#include "drivers/virtio_blk.h"
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

static void thread_a_entry(void *arg) {
    (void)arg;
    for (uint32_t i = 0; ; i++) {
        kprintf("[THREAD A] running (tick=%lu)\n", pit_get_ticks());
        /* Busy loop — will be preempted by timer */
        for (volatile int j = 0; j < 500000; j++) {}
    }
}

static void thread_b_entry(void *arg) {
    (void)arg;
    for (uint32_t i = 0; ; i++) {
        kprintf("[THREAD B] running (tick=%lu)\n", pit_get_ticks());
        /* Busy loop — will be preempted by timer */
        for (volatile int j = 0; j < 500000; j++) {}
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

    /* Phase 4: PCI bus enumeration */
    pci_init();

    /* Phase 4: VirtIO block device */
    if (virtio_blk_init() == 0) {
        uint8_t sector_buf[512];
        if (virtio_blk_read(0, 1, sector_buf) == 0) {
            kprintf("[VIRTIO-BLK] Sector 0 read OK. First 16 bytes:\n");
            kprintf("[VIRTIO-BLK] ");
            for (int i = 0; i < 16; i++) {
                kprintf("%x ", sector_buf[i]);
            }
            kprintf("\n");
            /* Check for MBR signature */
            if (sector_buf[510] == 0x55 && sector_buf[511] == 0xAA) {
                kprintf("[VIRTIO-BLK] MBR signature detected (0x55AA)\n");
            }
        } else {
            kprintf("[VIRTIO-BLK] Sector 0 read FAILED\n");
        }
    }

    /* Initialize threading — converts boot context to thread 0 */
    thread_init();

    /* Initialize scheduler */
    sched_init();

    /* Initialize process management */
    proc_init();

    /* Create test processes */
    Process *pa = proc_create(thread_a_entry, NULL);
    Process *pb = proc_create(thread_b_entry, NULL);
    if (pa == NULL || pb == NULL) {
        kprintf("[BOOT] FATAL: failed to create test processes\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Boot thread becomes the idle thread */
    sched_set_idle_thread(thread_current());

    /* Initialize PIT timer at 100 Hz */
    pit_init(100);

    /* Enable interrupts — PIT will start preempting */
    kprintf("[BOOT] Preemptive multitasking active.\n");
    __asm__ volatile ("sti");

    /* Idle loop — HLT wakes on interrupt, then halts again */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
