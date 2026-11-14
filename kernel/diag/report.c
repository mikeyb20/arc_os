/* arc_os — Boot report and diagnostic-mode pager (M1-04)
 *
 * Owns: the boot report (build commit, machine, firmware, CPU, APIC, TSC,
 * memory, display, ACPI tables, PCI devices) logged as "[REPORT]" lines on
 * every boot, and in diagnostic mode (arc.report / arc.selftest / arc.bench /
 * arc.qr) the on-screen pager that shows it page by page, every 10 s or on any
 * key, forever: as text, or as QR codes with arc.qr (diag/qr_mode.c).
 * Invariants: report lines are kept only in diagnostic mode (lines != NULL);
 * the pager owns the screen: the text pager removes the log's screen hook
 * first, the QR pager pauses the console instead (diag/qr_mode.c).
 * Locks: none. Runs on the BSP during boot; the pager is the only thing
 * running afterwards (init is not started in diagnostic mode).
 * Cost: boot only; O(memory map + PCI devices + ACPI tables) once. */

#include "diag/diag.h"
#include "arch/hal.h"
#include "drivers/acpi.h"
#include "drivers/pci.h"
#include "drivers/irq.h"
#include "drivers/smbios.h"
#include "drivers/fb_console.h"
#include "drivers/keyboard.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "time/clock.h"
#include "time/timer.h"
#include "lib/kprintf.h"
#include "lib/cmdline.h"
#include "lib/testmode.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "lib/build_commit.h"
#include <stdarg.h>

#define DIAG_MAX_LINES 320
#define DIAG_LINE_MAX  120
#define PAGER_SECONDS  10
#define MB             ((uint64_t)1024 * 1024)

static char (*lines)[DIAG_LINE_MAX];
static uint32_t line_count;

int diag_flag_on(const char *key) {
    char val[8];
    if (!cmdline_get(cmdline_raw(), key, val, sizeof(val))) return 0;
    return !(val[0] == '0' && val[1] == '\0');
}

int diag_requested(void) {
    return diag_flag_on("arc.report") || diag_flag_on("arc.selftest") || diag_flag_on("arc.bench") ||
           diag_flag_on("arc.qr");
}

uint32_t diag_line_count(void) {
    return lines != NULL ? line_count : 0;
}

const char *diag_line_text(uint32_t i) {
    return (lines != NULL && i < line_count) ? lines[i] : "";
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

/* The Features line, wrapped onto a second line at a space when the names
 * don't fit one report line. */
static void report_features(void) {
    char names[2 * DIAG_LINE_MAX];
    const size_t room = DIAG_LINE_MAX - 12;   /* after the 11-column label */
    hal_cpu_feature_names(names, sizeof(names));
    size_t len = strlen(names), cut = len;
    if (len > room) {
        cut = room;
        while (cut > 0 && names[cut] != ' ') cut--;
        if (cut == 0) cut = room;
    }
    char first[DIAG_LINE_MAX];   /* kprintf has no "%.*s" */
    memcpy(first, names, cut);
    first[cut] = '\0';
    diag_line("Features   %s", first);
    if (cut < len) diag_line("           %s", names + cut + (names[cut] == ' '));
}

static void report_cpu(const BootInfo *info, const AcpiInfo *acpi) {
    char buf[DIAG_LINE_MAX];
    uint32_t family, model, stepping;
    hal_cpu_brand(buf, sizeof(buf));
    hal_cpu_signature(&family, &model, &stepping);
    diag_line("CPU        %s", buf[0] ? buf : "(no brand string)");
    diag_line("CPU id     %s family 0x%x model 0x%x stepping %u", hal_cpu_vendor(), family, model, stepping);
    report_features();

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
    /* M1-06: what the hardware-sensitive clock choices came to on this machine */
    const ClockInfo *ci = clock_info();
    diag_line("Clock      %s timers (microcode 0x%x); TSCs %s on %u CPUs (worst step back %lu cycles)%s",
              ci->event_mode, t->microcode, ci->clamped ? "OUT OF STEP" : "in step", t->sync_cpus, t->max_warp,
              ci->rtc_ok ? "; wall clock from the RTC" : "; RTC unreadable: wall clock at 1970");
    /* M1-21: idle states, HWP and RAPL as probed on this machine */
    const PowerHwInfo *pw = hal_power_info();
    char hwp[40], rapl[40];
    if (pw->hwp)
        ksnprintf(hwp, sizeof(hwp), "HWP %u/%u%s", pw->hwp_cpus, hal_cpu_count(), pw->epp ? " EPP" : " no-EPP");
    else
        ksnprintf(hwp, sizeof(hwp), "no HWP");
    if (pw->rapl_mask)
        ksnprintf(rapl, sizeof(rapl), "RAPL%s%s%s 2^-%u J", pw->rapl_mask & 1 ? " pkg" : "",
                  pw->rapl_mask & 2 ? " cores" : "", pw->rapl_mask & 4 ? " psys" : "", pw->rapl_esu);
    else
        ksnprintf(rapl, sizeof(rapl), "no RAPL");
    diag_line("Power      idle %s %s%s%s%s%s%s; %s; %s; %s", pw->idle_method, pw->idle[0].name,
              pw->idle_count > 1 ? "+" : "", pw->idle_count > 1 ? pw->idle[1].name : "", pw->idle_why[0] ? " (" : "",
              pw->idle_why, pw->idle_why[0] ? ")" : "", pw->arat ? "ARAT" : "no ARAT", hwp, rapl);
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

/* Interrupt kind a driver got ("msix 2/3", "msi", "intx"), or what the
 * device offers when unbound. */
static const char *pci_irq_text(const PciDevice *d, char *buf, size_t size) {
    switch (d->irq_kind) {
    case IRQ_KIND_MSIX:
        ksnprintf(buf, size, "msix %u/%u", d->dev.nr_irqs, d->msix_entries);
        break;
    case IRQ_KIND_MSI:
        ksnprintf(buf, size, "msi");
        break;
    case IRQ_KIND_INTX:
        ksnprintf(buf, size, "intx %u", d->irq_line);
        break;
    default:
        ksnprintf(buf, size, "%s%s%s", d->cap_msix ? "msix " : "", d->cap_msi ? "msi " : "", d->irq_pin ? "intx" : "");
        break;
    }
    return buf;
}

/* PCI functions with their bound driver and interrupts; then each MSI-X
 * table, read and not driven (the M1-09 smoke-boot line: proves BAR sizing,
 * the MMIO window and the table layout on real hardware). */
static void report_pci(void) {
    int n = pci_get_device_count();
    diag_line("PCI        %d devices%s", n, pci_get_dropped_count() > 0 ? " (table full: more not listed)" : "");
    for (int i = 0; i < n; i++) {
        PciDevice *d = pci_get_device(i);
        const char *drv = pci_driver_name(d);
        char irq[40], tail[64];
        int len = ksnprintf(tail, sizeof(tail), "%-10s %s", drv ? drv : "no driver", pci_irq_text(d, irq, sizeof(irq)));
        if (len > (int)sizeof(tail) - 1) len = (int)sizeof(tail) - 1;
        while (len > 0 && tail[len - 1] == ' ') tail[--len] = '\0';   /* no trailing blanks */
        diag_line("  %02x:%02x.%x %04x:%04x class %02x%02x %-12s %s", d->addr.bus, d->addr.device, d->addr.function,
                  d->vendor_id, d->device_id, d->class_code, d->subclass, pci_class_name(d->class_code, d->subclass),
                  tail);
    }
    for (int i = 0; i < n; i++) {
        PciDevice *d = pci_get_device(i);
        uint16_t entries, masked;
        uint8_t bar;
        uint32_t off;
        if (d->cap_msix == 0) continue;
        if (pci_msix_info(d, &entries, &bar, &off, &masked) == 0) {
            diag_line("  MSI-X %02x:%02x.%x %u entries in BAR%u+0x%x (%lu KiB), %u masked", d->addr.bus, d->addr.device,
                      d->addr.function, entries, bar, off, d->bar_size[bar] / 1024, masked);
        } else {
            diag_line("  MSI-X %02x:%02x.%x %u entries in BAR%u+0x%x, table not readable (decode off)", d->addr.bus,
                      d->addr.device, d->addr.function, entries, bar, off);
        }
    }
}

void diag_report(const BootInfo *info) {
    if (diag_requested() && lines == NULL) {
        lines = kmalloc(sizeof(*lines) * DIAG_MAX_LINES, GFP_ZERO);
    }
    const AcpiInfo *acpi = acpi_get_info();
    diag_line("arc_os boot report, build %s", arc_build_commit);
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

static void wake_noop(void *arg) {
    (void)arg;   /* the interrupt itself ends the halt */
}

int diag_wait_key(uint32_t seconds, int can_halt) {
    uint32_t keys = keyboard_key_count();
    uint64_t end = clock_mono_ns() + (uint64_t)seconds * NSEC_PER_SEC;
    Timer wake;
    timer_init(&wake, wake_noop, NULL);
    while (clock_mono_ns() < end && keyboard_key_count() == keys) {
        if (!can_halt) {
            hal_cpu_relax();
            continue;
        }
        /* Arm on this CPU with interrupts off, check again, then halt until
         * the key or the timer: no wake-up can slip in between */
        hal_disable_interrupts();
        timer_arm(&wake, end);
        if (keyboard_key_count() == keys)
            hal_idle_wait();
        else
            hal_enable_interrupts();
    }
    timer_cancel_sync(&wake);
    return keyboard_key_count() != keys;
}

void diag_run(void) {
    keyboard_grab(1);   /* keys turn pages; nothing reads the TTY in this mode */

    int passes = 0, fails = 0;
    if (diag_flag_on("arc.selftest")) diag_selftest(&passes, &fails);
    if (diag_flag_on("arc.bench")) diag_bench();
    int qr = diag_flag_on("arc.qr");
    const char *hint = qr ? " Codes repeat; scan each one." : " Pages repeat; photograph each one.";
    diag_line("End of report.%s", testmode_active() ? "" : hint);

    if (testmode_active() && !qr) {
        kprintf("ARC_TEST DONE pass=%d fail=%d\n", passes, fails);
        testmode_exit(fails == 0 ? 0 : 1);
    }

    int can_halt = timer_cpu_ready();   /* a timer can end each halt */
    if (qr) diag_qr_run(can_halt, passes, fails);   /* returns only if it can't draw */
    if (fb_console_cols() == 0 || lines == NULL || fb_console_rows() < 2) {
        for (;;) hal_halt();   /* no screen: the report went to serial */
    }
    kprintf_set_putchar_hook(NULL);   /* the screen is the pager's now */
    uint32_t per_page = fb_console_rows() - 1;
    uint32_t pages = (line_count + per_page - 1) / per_page;
    for (uint32_t page = 0;; page = (page + 1) % pages) {
        pager_draw(page, pages, per_page);
        diag_wait_key(PAGER_SECONDS, can_halt);
    }
}
