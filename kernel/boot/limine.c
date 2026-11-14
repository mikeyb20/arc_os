#include <limine.h>
#include "boot/bootinfo.h"
#include "lib/mem.h"
#include "lib/string.h"

/* Limine request variables defined in limine_requests.c. */
extern volatile struct limine_memmap_request          memmap_request;
extern volatile struct limine_hhdm_request            hhdm_request;
extern volatile struct limine_framebuffer_request     framebuffer_request;
extern volatile struct limine_kernel_address_request  kernel_address_request;
extern volatile struct limine_rsdp_request            rsdp_request;
extern volatile struct limine_smbios_request smbios_request;
extern volatile struct limine_firmware_type_request firmware_type_request;
extern volatile struct limine_module_request          module_request;
extern volatile struct limine_kernel_file_request     kernel_file_request;
extern volatile struct limine_smp_request smp_request;

static BootInfo g_boot_info;

/* boot_start_cpu: Limine's record for BootInfo CPU i is smp->cpus[i] */
static boot_cpu_entry_t boot_cpu_entry;

/* CPU list from the SMP response (absent on some bootloaders: cpu_count 0). */
static void bootinfo_read_cpus(void) {
    struct limine_smp_response *smp = smp_request.response;
    if (smp == NULL) return;
    uint64_t count = smp->cpu_count;
    if (count > BOOTINFO_MAX_CPUS) count = BOOTINFO_MAX_CPUS;
    for (uint64_t i = 0; i < count; i++) {
        g_boot_info.cpu_apic_ids[i] = smp->cpus[i]->lapic_id;
    }
    g_boot_info.cpu_count = (uint32_t)count;
    g_boot_info.bsp_apic_id = smp->bsp_lapic_id;
}

/* Limine jumps here with its record; hand the kernel its argument. */
static void boot_cpu_trampoline(struct limine_smp_info *info) {
    boot_cpu_entry(info->extra_argument);
}

int boot_start_cpu(uint32_t i, boot_cpu_entry_t entry, uint64_t arg) {
    if (i >= g_boot_info.cpu_count || g_boot_info.cpu_apic_ids[i] == g_boot_info.bsp_apic_id) return -1;
    struct limine_smp_info *cpu = smp_request.response->cpus[i];
    boot_cpu_entry = entry;
    cpu->extra_argument = arg;
    /* The store to goto_address releases the CPU: everything above must be visible first */
    __atomic_store_n(&cpu->goto_address, boot_cpu_trampoline, __ATOMIC_SEQ_CST);
    return 0;
}

/* Firmware type and SMBIOS entry points (M1-04, both optional). With API
 * revision 0 the SMBIOS pointers may be HHDM pointers or physical addresses
 * depending on the Limine version: anything at or above the HHDM offset is a
 * pointer. Needs info->hhdm_offset. */
static void bootinfo_firmware(BootInfo *info) {
    struct limine_smbios_response *smb = smbios_request.response;
    if (smb != NULL) {
        uint64_t e32 = (uint64_t)(uintptr_t)smb->entry_32;
        uint64_t e64 = (uint64_t)(uintptr_t)smb->entry_64;
        if (e32 >= info->hhdm_offset) e32 -= info->hhdm_offset;
        if (e64 >= info->hhdm_offset) e64 -= info->hhdm_offset;
        info->smbios_entry32 = e32;
        info->smbios_entry64 = e64;
    }

    struct limine_firmware_type_response *fw = firmware_type_request.response;
    info->firmware = BOOT_FIRMWARE_UNKNOWN;
    if (fw != NULL) {
        switch (fw->firmware_type) {
        case LIMINE_FIRMWARE_TYPE_X86BIOS:
            info->firmware = BOOT_FIRMWARE_BIOS;
            break;
        case LIMINE_FIRMWARE_TYPE_UEFI32:
            info->firmware = BOOT_FIRMWARE_UEFI32;
            break;
        case LIMINE_FIRMWARE_TYPE_UEFI64:
            info->firmware = BOOT_FIRMWARE_UEFI64;
            break;
        default:
            break;
        }
    }
}

/* A Limine UUID (fields in host order) as GPT stores it: the first three
 * fields little-endian, then the 8 bytes as they are */
static void uuid_to_gpt(const struct limine_uuid *u, uint8_t out[16]) {
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(u->a >> (8 * i));
    for (int i = 0; i < 2; i++) out[4 + i] = (uint8_t)(u->b >> (8 * i));
    for (int i = 0; i < 2; i++) out[6 + i] = (uint8_t)(u->c >> (8 * i));
    for (int i = 0; i < 8; i++) out[8 + i] = u->d[i];
}

const BootInfo *bootinfo_init(void) {
    memset(&g_boot_info, 0, sizeof(g_boot_info));

    /* HHDM offset (critical). */
    struct limine_hhdm_response *hhdm = hhdm_request.response;
    if (hhdm == NULL) {
        return NULL;
    }
    g_boot_info.hhdm_offset = hhdm->offset;

    /* Memory map (critical). */
    struct limine_memmap_response *mmap = memmap_request.response;
    if (mmap == NULL) {
        return NULL;
    }
    uint64_t count = mmap->entry_count;
    if (count > BOOTINFO_MAX_MEMMAP_ENTRIES) {
        count = BOOTINFO_MAX_MEMMAP_ENTRIES;
    }
    for (uint64_t i = 0; i < count; i++) {
        struct limine_memmap_entry *entry = mmap->entries[i];
        g_boot_info.memory_map[i].base   = entry->base;
        g_boot_info.memory_map[i].length = entry->length;
        g_boot_info.memory_map[i].type   = (uint32_t)entry->type;
    }
    g_boot_info.memory_map_count = count;

    /* Framebuffer (optional). */
    struct limine_framebuffer_response *fb_resp = framebuffer_request.response;
    if (fb_resp != NULL && fb_resp->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_resp->framebuffers[0];
        g_boot_info.framebuffer.address          = fb->address;
        g_boot_info.framebuffer.width            = fb->width;
        g_boot_info.framebuffer.height           = fb->height;
        g_boot_info.framebuffer.pitch            = fb->pitch;
        g_boot_info.framebuffer.bpp              = fb->bpp;
        g_boot_info.framebuffer.red_mask_size    = fb->red_mask_size;
        g_boot_info.framebuffer.red_mask_shift   = fb->red_mask_shift;
        g_boot_info.framebuffer.green_mask_size  = fb->green_mask_size;
        g_boot_info.framebuffer.green_mask_shift = fb->green_mask_shift;
        g_boot_info.framebuffer.blue_mask_size   = fb->blue_mask_size;
        g_boot_info.framebuffer.blue_mask_shift  = fb->blue_mask_shift;
        g_boot_info.fb_present = true;
    }

    /* Kernel addresses (optional). */
    struct limine_kernel_address_response *kaddr = kernel_address_request.response;
    if (kaddr != NULL) {
        g_boot_info.kernel_phys_base = kaddr->physical_base;
        g_boot_info.kernel_virt_base = kaddr->virtual_base;
    }

    /* ACPI RSDP (optional). API revision 0 gives us a HHDM virtual pointer.
     * Convert to physical by subtracting the HHDM offset. */
    struct limine_rsdp_response *rsdp = rsdp_request.response;
    if (rsdp != NULL) {
        uint64_t virt = (uint64_t)(uintptr_t)rsdp->address;
        g_boot_info.acpi_rsdp = virt - g_boot_info.hhdm_offset;
    }

    bootinfo_firmware(&g_boot_info);

    /* Boot modules (init binary, etc.) */
    struct limine_module_response *mod_resp = module_request.response;
    if (mod_resp != NULL) {
        uint64_t mod_count = mod_resp->module_count;
        if (mod_count > BOOTINFO_MAX_MODULES) {
            mod_count = BOOTINFO_MAX_MODULES;
        }
        for (uint64_t i = 0; i < mod_count; i++) {
            struct limine_file *module = mod_resp->modules[i];
            g_boot_info.modules[i].address = module->address;
            g_boot_info.modules[i].size = module->size;
            if (module->path != NULL) {
                strncpy(g_boot_info.modules[i].path, module->path,
                        sizeof(g_boot_info.modules[i].path) - 1);
                g_boot_info.modules[i].path[sizeof(g_boot_info.modules[i].path) - 1] = '\0';
            }
        }
        g_boot_info.module_count = mod_count;
    }

    bootinfo_read_cpus();

    /* Kernel command line */
    g_boot_info.cmdline[0] = '\0';
    struct limine_kernel_file_response *kf = kernel_file_request.response;
    if (kf != NULL && kf->kernel_file != NULL && kf->kernel_file->cmdline != NULL) {
        strncpy(g_boot_info.cmdline, kf->kernel_file->cmdline,
                sizeof(g_boot_info.cmdline) - 1);
        g_boot_info.cmdline[sizeof(g_boot_info.cmdline) - 1] = '\0';
    }

    /* The boot disk and partition (GPT GUIDs; zero when not GPT) */
    g_boot_info.boot_gpt = false;
    memset(g_boot_info.boot_disk_guid, 0, sizeof(g_boot_info.boot_disk_guid));
    memset(g_boot_info.boot_part_guid, 0, sizeof(g_boot_info.boot_part_guid));
    if (kf != NULL && kf->kernel_file != NULL) {
        uuid_to_gpt(&kf->kernel_file->gpt_disk_uuid, g_boot_info.boot_disk_guid);
        uuid_to_gpt(&kf->kernel_file->gpt_part_uuid, g_boot_info.boot_part_guid);
        for (int i = 0; i < 16; i++) {
            if (g_boot_info.boot_disk_guid[i] != 0) g_boot_info.boot_gpt = true;
        }
    }

    return &g_boot_info;
}
