#include "arch/hal.h"
#include "arch/x86_64/serial.h"
#include "uapi/termios.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/pit.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "mm/mm.h"
#include "mm/filemap.h"
#include "boot/bootinfo.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/lockdep.h"
#include "proc/process.h"
#include "proc/init.h"
#include "drivers/acpi.h"
#include "drivers/pci.h"
#include "drivers/virtio_blk.h"
#include "drivers/blkdev.h"
#include "arc_disk.h"
#include "drivers/tty.h"
#include "drivers/keyboard.h"
#include "drivers/fb_console.h"
#include "security/hardening.h"
#include "drivers/virtio_net.h"
#include "drivers/irq.h"
#include "net/net.h"
#include "lib/kprintf.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "diag/diag.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/fat32.h"
#include "fs/devfs.h"
#include "fs/procfs.h"
#include "lib/cmdline.h"
#include "lib/testmode.h"
#include "lib/budget.h"
#include <stddef.h>
#include <stdint.h>

#include "drivers/ansi.h"
#include "drivers/vt.h"

/* Dual output: serial + ANSI parser → framebuffer. The screen is drawn as
 * console owner (lib/klog.h), so TTY text and kernel log lines drawn from
 * another CPU don't tear each other (M1-05). */
static void tty_dual_output(char c) {
    serial_putchar(c);
    int owned = klog_console_acquire();
    ansi_putchar(c);
    if (owned) klog_console_release();
}

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

/* arc.loglevel=N: the highest log level the consoles print (default 6, info) */
static void klog_setup_level(void) {
    char val[4];
    if (cmdline_get(cmdline_raw(), "arc.loglevel", val, sizeof(val)) && val[0] >= '0' && val[0] <= '7') {
        klog_set_console_level(val[0] - '0');
    }
}

/* Move the kernel log from its 16 KiB boot buffer to 256 KiB from the page
 * allocator (M1-04): kept for the life of the system, never freed. */
static void klog_setup_buffer(void) {
    struct page *pg = alloc_pages(KLOG_ORDER, GFP_KERNEL);
    if (pg == NULL) {
        kprintf("[KLOG] WARNING: no memory for the log ring, keeping 16 KiB\n");
        return;
    }
    klog_set_buffer(page_to_virt(pg), KLOG_SIZE);
}

/* Test mode only (suite kpanic, arc.panic=fault): write to the unmapped page
 * at 0 so the panic report shows a page fault, CR2 and this function by name
 * in the backtrace. */
static void __attribute__((noinline)) panic_test_fault(void) {
    kprintf("[TEST] kpanic suite: page fault on purpose\n");
    *(volatile uint64_t *)0x10 = 1;
}

/* Framebuffer console, when Limine gave a framebuffer. The cell array lets it
 * scroll without reading the framebuffer back (slow on real hardware). */
static void fb_setup(const BootInfo *info) {
    if (!info->fb_present || fb_console_init(&info->framebuffer) != 0) return;
    fb_console_attach_cells(kmalloc(fb_console_cells_bytes(), GFP_KERNEL));
    kprintf_set_putchar_hook(fb_console_putchar);
    diag_qr_panic_setup();
    panic_set_screen(diag_qr_panic_screen);   /* panic view plus a QR code of the report (M1-04c) */
    kprintf("[FB] Framebuffer console active (%lux%lu, %ux%u text)\n", info->framebuffer.width,
            info->framebuffer.height, fb_console_cols(), fb_console_rows());
}

/* Boot report (M1-04): logged on every boot. arc.report, arc.selftest and
 * arc.bench stop here and page it on screen instead of starting init. */
static void diag_setup(const BootInfo *info) {
    diag_report(info);
    if (!diag_requested()) return;
    hal_timer_init(100);
    hal_enable_interrupts();
    diag_run();
}

/* Test mode: budgets measured when init exits (testmode end hook) */
static void report_end_budgets(void) {
    ARC_BUDGET("slab_steps_max", slab_max_steps());
}

static void test_thread_entry(void *arg) {
    const char *label = (const char *)arg;
    for (;;) {
        kprintf("[%s] running (tick=%lu)\n", label, pit_get_ticks());
        /* Busy loop — will be preempted by timer */
        for (volatile int j = 0; j < 500000; j++) {}
    }
}

/* Print boot info: addresses, framebuffer, memory map, ACPI */
static void boot_log_info(const BootInfo *info) {
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
}

/* Run 1000 alloc/free cycles to verify heap integrity. */
static void heap_self_test(void) {
    kprintf("[BOOT] Running heap self-test...\n");
    for (int i = 0; i < 1000; i++) {
        size_t sz = 16 + (uint32_t)(i * 37) % 512;  /* Varying sizes 16-527 */
        void *p = kmalloc(sz, GFP_ZERO);
        if (p == NULL) {
            kprintf("[BOOT] FAIL: kmalloc returned NULL at iteration %d\n", i);
            KERNEL_PANIC();
        }
        /* Verify zero-fill */
        uint8_t *bytes = (uint8_t *)p;
        for (size_t j = 0; j < sz; j++) {
            if (bytes[j] != 0) {
                kprintf("[BOOT] FAIL: GFP_ZERO not zeroed at iteration %d\n", i);
                KERNEL_PANIC();
            }
        }
        /* Write pattern, then free */
        memset(p, 0xAB, sz);
        kfree(p);
    }
    /* Several slabs' worth of one class, then all freed: every empty slab
     * but the cache's one reserve goes back to the buddy allocator (this is
     * also the 3-step free that budget slab_steps_max allows) */
    static void *objs[200];
    uint64_t free_before = pmm_get_free_pages();
    for (int i = 0; i < 200; i++) {
        objs[i] = kmalloc(64, GFP_KERNEL);
        if (objs[i] == NULL) {
            kprintf("[BOOT] FAIL: kmalloc(64) returned NULL at %d\n", i);
            KERNEL_PANIC();
        }
    }
    for (int i = 0; i < 200; i++) kfree(objs[i]);
    if (pmm_get_free_pages() != free_before) {
        kprintf("[BOOT] FAIL: empty slabs kept %ld pages\n", (int64_t)(free_before - pmm_get_free_pages()));
        KERNEL_PANIC();
    }
    kprintf("[BOOT] Heap self-test passed (1000 alloc/free cycles, empty slabs returned)\n");
    kmalloc_dump_stats();
}

/* Check the buddy allocator after boot (M1-02): a 2 MiB block, a page below
 * 4 GiB, and a large kmalloc, all returned without leaking a page. */
static void pmm_self_test(void) {
    uint64_t free_before = pmm_get_free_pages();

    struct page *huge = alloc_pages(PAGE_ORDER_2MB, GFP_ZERO);
    if (huge == NULL || (page_to_phys(huge) & (0x200000ULL - 1)) != 0) {
        kprintf("[PMM] FAIL: no aligned 2 MiB block\n");
        KERNEL_PANIC();
    }
    uint8_t *bytes = page_to_virt(huge);
    if (bytes[0] != 0 || bytes[0x1FFFFF] != 0) {
        kprintf("[PMM] FAIL: 2 MiB block not zeroed\n");
        KERNEL_PANIC();
    }
    bytes[0] = 0x5A;
    bytes[0x1FFFFF] = 0xA5;
    uint64_t huge_phys = page_to_phys(huge);
    free_pages(huge, PAGE_ORDER_2MB);

    struct page *low = alloc_pages(0, GFP_DMA32);
    if (low == NULL || page_to_phys(low) >= 0x100000000ULL) {
        kprintf("[PMM] FAIL: GFP_DMA32 page missing or above 4 GiB\n");
        KERNEL_PANIC();
    }
    free_pages(low, 0);

    void *big = kmalloc(64 * 1024, GFP_ZERO);
    if (big == NULL) {
        kprintf("[PMM] FAIL: 64 KiB kmalloc\n");
        KERNEL_PANIC();
    }
    kfree(big);

    if (pmm_get_free_pages() != free_before) {
        kprintf("[PMM] FAIL: self-test leaked %ld pages\n", (int64_t)(free_before - pmm_get_free_pages()));
        KERNEL_PANIC();
    }
    kprintf("[PMM] Self-test passed: 2 MiB block at 0x%lx\n", huge_phys);
}

/* Create path holding data, logging failures (boot files). */
static void boot_write_file(const char *path, const void *data, uint64_t size, uint32_t mode) {
    int err = vfs_write_file(path, data, size, mode);
    if (err != 0) kprintf("[VFS] FAIL: %s (%d)\n", path, err);
}

/* Mount sb over a new directory at path. */
static void boot_mount(const char *path, SuperBlock *sb, const char *source) {
    int err = sb != NULL ? vfs_mkdir(path, 0755) : -ENOMEM;
    if (err == 0) err = vfs_mount(path, sb, source);
    if (err == 0) {
        kprintf("[VFS] Mounted %s at %s\n", sb->type, path);
    } else {
        kprintf("[VFS] FAIL: mount %s (%d)\n", path, err);
    }
}

/* Programs that stay only in /boot (the rest are copied to /bin) */
static int boot_only(const char *name) {
    static const char *const names[] = {"kernel.elf", "init", "login", "shell", "hello", "echo"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) return 1;
    }
    return 0;
}

/* Initialize the VFS with a ramfs root, load boot modules, create /etc,
 * /home, /tmp, /bin and mount devfs and procfs. */
static void vfs_setup(const BootInfo *info) {
    vfs_init();
    if (vfs_mount_root(ramfs_create_sb(), "ramfs") != 0) {
        kprintf("[VFS] FAIL: no root filesystem\n");
        KERNEL_PANIC();
    }
    kprintf("[VFS] Initialized with ramfs root\n");

    /* Boot modules into ramfs; programs are executable */
    vfs_mkdir("/boot", 0755);
    vfs_mkdir("/bin", 0755);
    for (uint64_t i = 0; i < info->module_count; i++) {
        const char *path = info->modules[i].path;
        boot_write_file(path, info->modules[i].address, info->modules[i].size, 0755);
        kprintf("[BOOT] Loaded module '%s' (%lu bytes) into ramfs\n", path, info->modules[i].size);
        if (strncmp(path, "/boot/", 6) == 0 && strchr(path + 6, '/') == NULL && !boot_only(path + 6)) {
            char dst[VFS_NAME_MAX + 6];
            ksnprintf(dst, sizeof(dst), "/bin/%s", path + 6);
            boot_write_file(dst, info->modules[i].address, info->modules[i].size, 0755);
        }
    }
    kprintf("[VFS] Created /bin with coreutils\n");

    vfs_mkdir("/etc", 0755);
    boot_write_file("/etc/hostname", "arc_os\n", 7, 0644);
    static const char passwd[] = "root::0:0:root:/:/boot/shell\n"
                                 "user:user:1000:1000:user:/home/user:/boot/shell\n";
    boot_write_file("/etc/passwd", passwd, sizeof(passwd) - 1, 0644);
    vfs_mkdir("/home", 0755);
    vfs_mkdir("/home/user", 0755);
    /* /tmp exists on every Unix system; programs expect it (M0-10: a suite
     * creating it showed up as leaked heap) */
    vfs_mkdir("/tmp", 01777);

    boot_mount("/dev", devfs_init(), "devfs");
    boot_mount("/proc", procfs_init(), "proc");
}

/* FAT32 at /disk, read-only: the first disk's EFI system partition when it
 * has a GPT, else the whole disk (test images). M1-13 mounts the arc_os
 * partitions of the boot disk. */
static void disk_mount(void) {
    BlockDevice *dev = blk_find("vda");
    if (!dev) {
        kprintf("[BLK] No block device registered\n");
        return;
    }
    if (dev->has_gpt) {
        Guid esp;
        gpt_guid_parse(ARC_PART_TYPE_ESP, &esp);
        dev = blk_find_part(&dev->disk_guid, &esp);
        if (!dev) {
            kprintf("[BLK] vda: GPT without an EFI system partition\n");
            return;
        }
    }

    SuperBlock *fat = fat32_mount(dev);
    if (fat != NULL) {
        boot_mount("/disk", fat, dev->name);
    } else {
        kprintf("[BLK] No FAT32 filesystem found on %s\n", dev->name);
    }
}

/* Request timeouts (ADR-0005): checked on the timer tick until M1-06 */
static void blk_tick(void) {
    blk_expire_all(hal_tsc_read());
}

/* Driver model (M1-09): IRQ layer, the PCI bus, then the drivers, which bind
 * as they register. After hal_smp_init: MSI needs the local APIC, and
 * interrupts are spread over every online CPU. Then mount the virtio disk and
 * bring up the network on what bound. */
static void devices_setup(void) {
    irq_init();
    pci_bus_init();
    virtio_blk_register();
    if (virtio_net_register() > 0) net_init();
    if (testmode_active() && strcmp(testmode_suite(), "devices") == 0) {
        devtest_register();
        blktest_run();
    }
    disk_mount();
}

/* Stop on a CPU below x86-64-v3 (ADR-0003), saying why on serial and, when
 * there is one, the framebuffer. Runs before vmm_init: the bootloader's page
 * tables still map the framebuffer. */
static void cpu_floor_check(const BootInfo *info) {
    char missing[160];
    if (cpu_floor_missing(missing, sizeof(missing)) == 0) return;
    if (info->fb_present && fb_console_init(&info->framebuffer) == 0) kprintf_set_putchar_hook(fb_console_putchar);
    kprintf("[BOOT] CPU floor: arc_os needs an x86-64-v3 CPU (AVX2, FMA, BMI2, XSAVE).\n"
            "[BOOT] CPU floor: this CPU lacks: %s\n",
            missing);
    if (testmode_active()) testmode_exit(TESTMODE_CPU_FLOOR_CODE);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void kmain(void) {
    hal_cpu_early_init();   /* GS: spinlocks count their depth per CPU */
    serial_init();
    hardening_init();
    serial_puts("[BOOT] arc_os kernel booting...\n");

    const BootInfo *info = bootinfo_init();
    if (info == NULL) {
        serial_puts("[BOOT] FATAL: failed to parse boot info\n");
        KERNEL_PANIC();
    }

    /* Kernel command line; arc.test=<suite> turns on test mode (M0-02) */
    cmdline_set(info->cmdline);
    testmode_init(info->cmdline);
    klog_setup_level();
    kprintf("[BOOT] Command line: '%s'\n", cmdline_raw());
    if (testmode_active()) {
        kprintf("[TEST] Kernel test mode, suite '%s'\n", testmode_suite());
    }

    boot_log_info(info);

    serial_puts("[BOOT] stage: CPU\n");
    cpu_features_init();   /* before the per-CPU block: it records the core type */
    cpu_floor_check(info);
    hal_tsc_calibrate();   /* 50 ms against the PIT: boot report, self-test, benchmarks */
    serial_puts("[BOOT] stage: GDT, per-CPU, IDT\n");
    hal_cpu_init();   /* GS -> the BSP's per-CPU block: thread_current() works from here */
    serial_puts("[BOOT] stage: FPU\n");
    fpu_cpu_init(1);   /* before any thread exists: threads copy its clean image */
    serial_puts("[BOOT] stage: PIC\n");
    pic_init();
    serial_puts("[BOOT] stage: PMM\n");
    pmm_init(info);
    serial_puts("[BOOT] stage: VMM\n");
    vmm_init(info);
    serial_puts("[BOOT] stage: heap\n");
    kmalloc_init();
    mm_init();   /* VMA and address-space caches (M1-07) */
    filemap_init();   /* page-cache radix nodes and mappings (M1-12) */
    klog_setup_buffer();
    heap_self_test();
    pmm_self_test();
    fb_setup(info);

    /* Initialize ANSI parser and VT subsystem */
    ansi_init();
    if (info->fb_present && fb_console_cols() > 0) {
        uint32_t vt_cols = fb_console_cols();
        uint32_t vt_rows = fb_console_rows();
        vt_init(vt_cols, vt_rows);
        struct winsize ws = { .ws_row = (uint16_t)vt_rows, .ws_col = (uint16_t)vt_cols };
        tty_set_winsize(&ws);   /* TIOCGWINSZ reports the console's size */
    }

    serial_puts("[BOOT] stage: VFS\n");
    vfs_setup(info);
    serial_puts("[BOOT] stage: ACPI\n");
    acpi_init(info->acpi_rsdp);
    serial_puts("[BOOT] stage: PCI\n");
    pci_init();   /* enumerate and size BARs; drivers bind after the APIC switch */

    /* Initialize threading — converts boot context to thread 0 */
    thread_init();
    sched_init();
    proc_init();
    syscall_init();

    /* Initialize TTY and PS/2 keyboard (TTY first — keyboard handler calls tty_input_char) */
    tty_init();
    /* Route TTY output to both serial and framebuffer */
    if (info->fb_present) {
        tty_set_output(tty_dual_output);
    }
    keyboard_init();

    /* APIC interrupt controllers, then the other CPUs (they idle until M1-05) */
    {
        const AcpiInfo *acpi = acpi_get_info();
        if (acpi && acpi->local_apic_address != 0) {
            hal_smp_init(info, acpi);
        }
    }

    serial_puts("[BOOT] stage: devices\n");
    devices_setup();

    /* Test mode self-check: suite "kpanic" proves a panic ends the run at once;
     * with arc.panic=fault it is a real page fault instead (M1-04) */
    if (testmode_active() && strcmp(testmode_suite(), "kpanic") == 0) {
        if (cmdline_get(cmdline_raw(), "arc.panic", NULL, 0)) panic_test_fault();
        kprintf("[TEST] kpanic suite: panicking on purpose\n");
        KERNEL_PANIC();
    }
    /* Suite "lockorder" (M1-05): the lock checker must stop an inverted pair */
    if (testmode_active() && strcmp(testmode_suite(), "lockorder") == 0) lockdep_test_inversion();

    diag_setup(info);

    /* Boot budgets (ADR-0019): memory left for user space and what the kernel took */
    if (testmode_active()) {
        HeapStats hs;
        kmalloc_get_stats(&hs);
        ARC_BUDGET("boot_free_bytes", pmm_get_free_pages() * PAGE_SIZE);
        ARC_BUDGET("kernel_heap_bytes", hs.total_used + hs.large_bytes);
        ARC_BUDGET("page_descriptor_bytes", pmm_get_descriptor_bytes());
        testmode_set_end_hook(report_end_budgets);
    }

    /* Launch init process from boot module */
    if (init_launch(info) != 0) {
        kprintf("[BOOT] WARNING: init_launch failed, falling back to test threads\n");
        Process *pa = proc_create(test_thread_entry, (void *)"THREAD A");
        Process *pb = proc_create(test_thread_entry, (void *)"THREAD B");
        if (pa == NULL || pb == NULL) {
            kprintf("[BOOT] FATAL: failed to create test processes\n");
            KERNEL_PANIC();
        }
    }

    /* PIT at 100 Hz: uptime ticks (M1-06 replaces it). Slices come from each
     * CPU's own timer, armed by the scheduler. */
    hal_timer_set_tick_hook(blk_tick);
    pit_init(100);

    /* The boot thread becomes CPU 0's idle thread, the APs join in */
    kprintf("[BOOT] Preemptive multitasking active.\n");
    sched_start();
}
