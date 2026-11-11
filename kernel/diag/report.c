/* arc_os — Boot report and diagnostic-mode pager (M1-04)
 *
 * Owns: the boot report (machine, firmware, CPU, APIC, TSC, memory, display,
 * ACPI tables, PCI devices) logged as "[REPORT]" lines on every boot, and in
 * diagnostic mode (arc.report / arc.selftest / arc.bench) the on-screen pager
 * that shows it page by page, every 10 s or on any key, forever.
 * Invariants: report lines are kept only in diagnostic mode (lines != NULL);
 * the pager owns the screen: the log's screen hook is removed first.
 * Locks: none. Runs on the BSP during boot; the pager is the only thing
 * running afterwards (init is not started in diagnostic mode).
 * Cost: boot only; O(memory map + PCI devices + ACPI tables) once. */

#include "diag/diag.h"
#include "arch/hal.h"
#include "drivers/acpi.h"
#include "drivers/pci.h"
#include "drivers/smbios.h"
#include "drivers/fb_console.h"
#include "drivers/keyboard.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/cmdline.h"
#include "lib/testmode.h"
#include "lib/string.h"
#include "lib/mem.h"
#include <stdarg.h>

#define DIAG_MAX_LINES 320
#define DIAG_LINE_MAX  120
#define PAGER_SECONDS  10
#define MB             ((uint64_t)1024 * 1024)

static char (*lines)[DIAG_LINE_MAX];
static uint32_t line_count;

/* Command-line flag "key=1" (any value but 0 counts; a bare key too). */
static int flag_on(const char *key) {
    char val[8];
    if (!cmdline_get(cmdline_raw(), key, val, sizeof(val))) return 0;
    return !(val[0] == '0' && val[1] == '\0');
}

int diag_requested(void) {
    return flag_on("arc.report") || flag_on("arc.selftest") || flag_on("arc.bench");
}

void diag_line(const char *fmt, ...) {
    char buf[DIAG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf("[REPORT] %s\n", buf);
    if (lines != NULL && line_count < DIAG_MAX_LINES) memcpy(lines[line_count++], buf, DIAG_LINE_MAX);
}

void diag_delay_ms(uint32_t ms) {
    uint64_t khz = hal_tsc_info()->khz;
    uint64_t cycles = khz != 0 ? khz * ms : (uint64_t)ms * 3000000;   /* unknown: assume 3 GHz */
    uint64_t start = hal_tsc_read();
    while (hal_tsc_read() - start < cycles) hal_cpu_relax();
}

static const char *firmware_name(uint32_t fw) {
    switch (fw) {
    case BOOT_FIRMWARE_BIOS:
        return "legacy BIOS";
    case BOOT_FIRMWARE_UEFI32:
        return "UEFI 32-bit";
    case BOOT_FIRMWARE_UEFI64:
        return "UEFI 64-bit";
    default:
        return "unknown";
    }
}

static void report_machine(const BootInfo *info) {
    SmbiosInfo s;
    if (smbios_read(info->smbios_entry32, info->smbios_entry64, &s) != 0) {
        diag_line("Machine    unknown (no SMBIOS table)");
        diag_line("Firmware   %s", firmware_name(info->firmware));
        return;
    }
    diag_line("Machine    %s %s%s%s%s", s.maker, s.model, s.model_version[0] ? " (" : "", s.model_version,
              s.model_version[0] ? ")" : "");
    diag_line("Firmware   %s; BIOS %s %s %s; SMBIOS %u.%u", firmware_name(info->firmware), s.bios_vendor,
              s.bios_version, s.bios_date, s.major, s.minor);
}

static void report_cpu(const BootInfo *info, const AcpiInfo *acpi) {
    char buf[DIAG_LINE_MAX];
    uint32_t family, model, stepping;
    hal_cpu_brand(buf, sizeof(buf));
    hal_cpu_signature(&family, &model, &stepping);
    diag_line("CPU        %s", buf[0] ? buf : "(no brand string)");
    diag_line("CPU id     %s family 0x%x model 0x%x stepping %u", hal_cpu_vendor(), family, model, stepping);
    hal_cpu_feature_names(buf, sizeof(buf));
    diag_line("Features   %s", buf);

    uint32_t p = 0, e = 0, other = 0;
    for (uint32_t i = 0; i < HAL_MAX_CPUS; i++) {
        uint32_t apic;
        const char *core;
        if (hal_cpu_info(i, &apic, &core) != 0) continue;
        if (core[0] == 'P')
            p++;
        else if (core[0] == 'E')
            e++;
        else
            other++;
    }
    diag_line("CPUs       %u online; MADT lists %u, bootloader %u; cores: %u P, %u E, %u untyped", hal_cpu_count(),
              acpi ? acpi->cpu_count : 0, info->cpu_count, p, e, other);
    if (acpi == NULL) {
        diag_line("APIC       not set up (no ACPI MADT): legacy PIC");
    } else {
        diag_line("APIC       %s mode%s%s", hal_apic_mode(), acpi->dmar_present ? "; DMAR x2APIC opt-out: " : "",
                  acpi->dmar_present ? ((acpi->dmar_flags & ACPI_DMAR_X2APIC_OPT_OUT) ? "SET" : "no") : "");
    }
    const TscInfo *t = hal_tsc_info();
    diag_line("TSC        %lu kHz from %s (CPUID 0x15 %lu kHz, PIT %lu kHz, base %lu MHz)%s%s", t->khz, t->source,
              t->cpuid_khz, t->pit_khz, t->base_mhz, t->invariant ? ", invariant" : ", NOT invariant",
              t->deadline ? ", deadline timer" : "");
}

static void report_memory(const BootInfo *info) {
    uint64_t usable = 0, acpi = 0, reserved = 0, other = 0, top = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const MemoryMapEntry *m = &info->memory_map[i];
        switch (m->type) {
        case MEMMAP_USABLE:
        case MEMMAP_BOOTLOADER_RECLAIM:
        case MEMMAP_KERNEL_AND_MODULES:
            usable += m->length;
            if (m->base + m->length > top) top = m->base + m->length;
            break;
        case MEMMAP_ACPI_RECLAIMABLE:
        case MEMMAP_ACPI_NVS:
            acpi += m->length;
            break;
        case MEMMAP_RESERVED:
            reserved += m->length;
            break;
        default:
            other += m->length;
            break;
        }
    }
    diag_line("Memory     %lu MB RAM (top of RAM 0x%lx), ACPI %lu KB, reserved %lu MB, other %lu MB, %lu map entries",
              usable / MB, top, acpi / 1024, reserved / MB, other / MB, info->memory_map_count);
    diag_line("Memory     %lu MB free now", pmm_get_free_pages() * (uint64_t)PAGE_SIZE / MB);
}

static void report_display(const BootInfo *info) {
    if (!info->fb_present) {
        diag_line("Display    no framebuffer");
        return;
    }
    const Framebuffer *f = &info->framebuffer;
    diag_line("Display    %lux%lu, %u bpp, pitch %lu; R %u@%u G %u@%u B %u@%u; text %ux%u", f->width, f->height, f->bpp,
              f->pitch, f->red_mask_size, f->red_mask_shift, f->green_mask_size, f->green_mask_shift, f->blue_mask_size,
              f->blue_mask_shift, fb_console_cols(), fb_console_rows());
}

static void report_acpi(const AcpiInfo *acpi) {
    if (acpi == NULL) {
        diag_line("ACPI       none found");
        return;
    }
    diag_line("ACPI       revision %u, OEM '%s', %u tables%s", acpi->rsdp_revision, acpi->oem_id, acpi->tables_listed,
              acpi->tables_listed > acpi->table_count ? " (first ones listed)" : "");
    char buf[DIAG_LINE_MAX];
    int pos = 0;
    for (uint32_t i = 0; i < acpi->table_count; i++) {
        const AcpiTableEntry *t = &acpi->tables[i];
        pos += ksnprintf(buf + pos, sizeof(buf) - (size_t)pos, " %s:%u%s", t->signature, t->length,
                         t->checksum_ok ? "" : "(BAD)");
        if (pos > 80 || i + 1 == acpi->table_count) {
            diag_line("          %s", buf);
            pos = 0;
        }
    }
}

static void report_pci(void) {
    int n = pci_get_device_count();
    diag_line("PCI        %d devices%s", n, pci_get_dropped_count() > 0 ? " (table full: more not listed)" : "");
    for (int i = 0; i < n; i++) {
        const PciDevice *d = pci_get_device(i);
        diag_line("  %02x:%02x.%x %04x:%04x class %02x%02x %-12s %s", d->addr.bus, d->addr.device, d->addr.function,
                  d->vendor_id, d->device_id, d->class_code, d->subclass, pci_class_name(d->class_code, d->subclass),
                  d->driver ? d->driver : "no driver");
    }
}

void diag_report(const BootInfo *info) {
    if (diag_requested() && lines == NULL) {
        lines = kmalloc(sizeof(*lines) * DIAG_MAX_LINES, GFP_ZERO);
    }
    const AcpiInfo *acpi = acpi_get_info();
    diag_line("arc_os boot report (M1-04 bring-up)");
    report_machine(info);
    report_cpu(info, acpi);
    report_memory(info);
    report_display(info);
    diag_line("Cmdline    %s", cmdline_raw());
    report_acpi(acpi);
    report_pci();
}

/* ---- Pager ---- */

/* Draw page `page` of `pages` (rows - 1 lines under a banner). */
static void pager_draw(uint32_t page, uint32_t pages, uint32_t per_page) {
    char banner[DIAG_LINE_MAX];
    ksnprintf(banner, sizeof(banner), " arc_os boot report  page %u/%u  next in %u s, any key: next page", page + 1,
              pages, PAGER_SECONDS);
    fb_console_write_row(0, banner, 0x000000, 0x5FD7FF);
    for (uint32_t r = 0; r < per_page; r++) {
        uint32_t idx = page * per_page + r;
        const char *s = idx < line_count ? lines[idx] : "";
        /* PASS/FAIL lines in colour so they show up in a photo */
        uint32_t color = 0xFFFFFF;
        if (strncmp(s, "SELFTEST PASS", 13) == 0) color = 0x5FFF5F;
        if (strncmp(s, "SELFTEST FAIL", 13) == 0) color = 0xFF5F5F;
        fb_console_write_row(r + 1, s, color, 0x000000);
    }
}

/* Wait up to `seconds` or until a key is pressed. Halts between interrupts when
 * the timer is known to tick; otherwise polls (no wake-up source to trust). */
static void pager_wait(uint32_t seconds, int can_halt) {
    uint32_t keys = keyboard_key_count();
    uint64_t khz = hal_tsc_info()->khz ? hal_tsc_info()->khz : 3000000;
    uint64_t cycles = khz * 1000 * seconds;
    uint64_t start = hal_tsc_read();
    while (hal_tsc_read() - start < cycles && keyboard_key_count() == keys) {
        if (can_halt)
            hal_halt();
        else
            hal_cpu_relax();
    }
}

void diag_run(void) {
    keyboard_grab(1);   /* keys turn pages; nothing reads the TTY in this mode */
    uint64_t ticks = hal_timer_ticks();

    int passes = 0, fails = 0;
    if (flag_on("arc.selftest")) diag_selftest(&passes, &fails);
    if (flag_on("arc.bench")) diag_bench();
    diag_line("End of report.%s", testmode_active() ? "" : " Pages repeat; photograph each one.");

    if (testmode_active()) {
        kprintf("ARC_TEST DONE pass=%d fail=%d\n", passes, fails);
        testmode_exit(fails == 0 ? 0 : 1);
    }

    int can_halt = hal_timer_ticks() != ticks;   /* the PIT woke us during the tests */
    if (!can_halt) {
        diag_delay_ms(100);
        can_halt = hal_timer_ticks() != ticks;
    }
    if (fb_console_cols() == 0 || lines == NULL || fb_console_rows() < 2) {
        for (;;) hal_halt();   /* no screen: the report went to serial */
    }
    kprintf_set_putchar_hook(NULL);   /* the screen is the pager's now */
    uint32_t per_page = fb_console_rows() - 1;
    uint32_t pages = (line_count + per_page - 1) / per_page;
    for (uint32_t page = 0;; page = (page + 1) % pages) {
        pager_draw(page, pages, per_page);
        pager_wait(PAGER_SECONDS, can_halt);
    }
}
