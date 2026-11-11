#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#define LIMINE_SECTION __attribute__((used, section(".limine_requests")))

/*
 * Start marker, base revision, and end marker must live in .limine_requests.
 * We define them manually because the LIMINE_* macros expand to full variable
 * definitions and cannot have a section attribute applied.
 */

LIMINE_SECTION
volatile uint64_t limine_requests_start_marker[4] = {
    0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
    0x785c6ed015d3e316, 0x181e920a7852b9d9
};

LIMINE_SECTION
volatile uint64_t limine_base_revision[3] = {
    0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 0
};

/* Framebuffer — needed for console output */
LIMINE_SECTION
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL
};

/* Memory map — needed for PMM */
LIMINE_SECTION
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL
};

/* Higher-half direct map offset */
LIMINE_SECTION
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL
};

/* ACPI RSDP pointer */
LIMINE_SECTION
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = NULL
};

/* Kernel physical/virtual base addresses */
LIMINE_SECTION
volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = NULL
};

/* Boot modules (init binary, etc.) */
LIMINE_SECTION
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL
};

/* Kernel file — gives the kernel command line (limine.conf `cmdline:`) */
LIMINE_SECTION
volatile struct limine_kernel_file_request kernel_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
    .response = NULL
};

/* SMP — for multi-core AP bringup. X2APIC: Limine switches every CPU to x2APIC
 * mode when the CPU has it, so it can start CPUs whose APIC ID is above 255; the
 * kernel picks the same mode (lapic.c, M1-03). */
LIMINE_SECTION
volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST, .revision = 0, .response = NULL, .flags = LIMINE_SMP_X2APIC};

/* SMBIOS entry points and firmware type, for the boot report (M1-04) */
LIMINE_SECTION
volatile struct limine_smbios_request smbios_request = {.id = LIMINE_SMBIOS_REQUEST, .revision = 0, .response = NULL};

LIMINE_SECTION
volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST, .revision = 0, .response = NULL};

LIMINE_SECTION
volatile uint64_t limine_requests_end_marker[2] = {
    0xadc0e0531bb10d03, 0x9572709f31764c62
};
