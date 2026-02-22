#include <limine.h>
#include "boot/bootinfo.h"
#include "lib/mem.h"

/* Limine request variables defined in limine_requests.c. */
extern volatile struct limine_memmap_request          memmap_request;
extern volatile struct limine_hhdm_request            hhdm_request;
extern volatile struct limine_framebuffer_request     framebuffer_request;
extern volatile struct limine_kernel_address_request  kernel_address_request;
extern volatile struct limine_rsdp_request            rsdp_request;

static BootInfo g_boot_info;

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
        struct limine_memmap_entry *e = mmap->entries[i];
        g_boot_info.memory_map[i].base   = e->base;
        g_boot_info.memory_map[i].length = e->length;
        g_boot_info.memory_map[i].type   = (uint32_t)e->type;
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

    return &g_boot_info;
}
