/* arc_os — Host-side tests for kernel/boot/limine.c (bootinfo_init) */

#include "test_framework.h"
#include <stdint.h>
#include <stdbool.h>

/* Guard kernel headers */
#define ARCHOS_LIB_MEM_H    /* Use libc memset */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_STRING_H /* Use libc strncpy */

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include limine.h for struct definitions (it only needs stdint.h).
 * We use the relative path, then define the guard so limine.c's
 * #include <limine.h> becomes a no-op. */
#include "../kernel/include/limine.h"
#include "../kernel/boot/bootinfo.h"

/* Guard the angle-bracket include in limine.c — we already included it above.
 * limine.h uses LIMINE_H as its guard. It's already defined. */

/* We define the request variables ourselves (same as limine_requests.c) */
volatile struct limine_memmap_request         memmap_request;
volatile struct limine_hhdm_request           hhdm_request;
volatile struct limine_framebuffer_request    framebuffer_request;
volatile struct limine_kernel_address_request kernel_address_request;
volatile struct limine_rsdp_request           rsdp_request;
volatile struct limine_module_request        module_request;

/* Include the real limine.c */
#include "../kernel/boot/limine.c"

/* --- Test helpers --- */

static struct limine_hhdm_response test_hhdm_resp;
static struct limine_memmap_response test_mmap_resp;
static struct limine_memmap_entry test_mmap_entries[80];
static struct limine_memmap_entry *test_mmap_entry_ptrs[80];
static struct limine_framebuffer_response test_fb_resp;
static struct limine_framebuffer test_fb;
static struct limine_framebuffer *test_fb_ptr;
static struct limine_kernel_address_response test_kaddr_resp;
static struct limine_rsdp_response test_rsdp_resp;

static void reset_bootinfo_state(void) {
    memset(&g_boot_info, 0, sizeof(g_boot_info));

    memset(&test_hhdm_resp, 0, sizeof(test_hhdm_resp));
    memset(&test_mmap_resp, 0, sizeof(test_mmap_resp));
    memset(test_mmap_entries, 0, sizeof(test_mmap_entries));
    memset(test_mmap_entry_ptrs, 0, sizeof(test_mmap_entry_ptrs));
    memset(&test_fb_resp, 0, sizeof(test_fb_resp));
    memset(&test_fb, 0, sizeof(test_fb));
    memset(&test_kaddr_resp, 0, sizeof(test_kaddr_resp));
    memset(&test_rsdp_resp, 0, sizeof(test_rsdp_resp));

    /* Default: valid HHDM + valid memmap with 0 entries */
    test_hhdm_resp.offset = 0xFFFF800000000000ULL;
    hhdm_request.response = &test_hhdm_resp;

    test_mmap_resp.entry_count = 0;
    test_mmap_resp.entries = test_mmap_entry_ptrs;
    memmap_request.response = &test_mmap_resp;

    framebuffer_request.response = NULL;
    kernel_address_request.response = NULL;
    rsdp_request.response = NULL;
    module_request.response = NULL;
}

static void setup_mmap_entries(int count) {
    for (int i = 0; i < count; i++) {
        test_mmap_entry_ptrs[i] = &test_mmap_entries[i];
    }
    test_mmap_resp.entry_count = (uint64_t)count;
    test_mmap_resp.entries = test_mmap_entry_ptrs;
}

/* --- Tests --- */

TEST(null_hhdm_returns_null) {
    reset_bootinfo_state();
    hhdm_request.response = NULL;
    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info == NULL);
    return 0;
}

TEST(null_memmap_returns_null) {
    reset_bootinfo_state();
    memmap_request.response = NULL;
    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info == NULL);
    return 0;
}

TEST(valid_memmap) {
    reset_bootinfo_state();
    test_mmap_entries[0] = (struct limine_memmap_entry){ .base = 0x1000, .length = 0x2000, .type = 0 };
    test_mmap_entries[1] = (struct limine_memmap_entry){ .base = 0x100000, .length = 0x400000, .type = 1 };
    test_mmap_entries[2] = (struct limine_memmap_entry){ .base = 0xFE000000, .length = 0x1000000, .type = 7 };
    setup_mmap_entries(3);

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->memory_map_count, 3);
    ASSERT_EQ(info->memory_map[0].base, 0x1000);
    ASSERT_EQ(info->memory_map[0].length, 0x2000);
    ASSERT_EQ(info->memory_map[0].type, 0);
    ASSERT_EQ(info->memory_map[1].base, 0x100000);
    ASSERT_EQ(info->memory_map[1].length, 0x400000);
    ASSERT_EQ(info->memory_map[1].type, 1);
    ASSERT_EQ(info->memory_map[2].base, 0xFE000000);
    ASSERT_EQ(info->memory_map[2].length, 0x1000000);
    ASSERT_EQ(info->memory_map[2].type, 7);
    return 0;
}

TEST(entry_count_clamped) {
    reset_bootinfo_state();
    /* Create 80 entries — should be clamped to 64 */
    for (int i = 0; i < 80; i++) {
        test_mmap_entries[i].base = (uint64_t)i * 0x1000;
        test_mmap_entries[i].length = 0x1000;
        test_mmap_entries[i].type = 0;
    }
    setup_mmap_entries(80);

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->memory_map_count, 64);
    return 0;
}

TEST(framebuffer_absent) {
    reset_bootinfo_state();
    framebuffer_request.response = NULL;
    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_FALSE(info->fb_present);
    return 0;
}

TEST(framebuffer_present) {
    reset_bootinfo_state();

    test_fb.address = (void *)0xFD000000;
    test_fb.width = 1920;
    test_fb.height = 1080;
    test_fb.pitch = 7680;
    test_fb.bpp = 32;
    test_fb.red_mask_size = 8;
    test_fb.red_mask_shift = 16;
    test_fb.green_mask_size = 8;
    test_fb.green_mask_shift = 8;
    test_fb.blue_mask_size = 8;
    test_fb.blue_mask_shift = 0;

    test_fb_ptr = &test_fb;
    test_fb_resp.framebuffer_count = 1;
    test_fb_resp.framebuffers = &test_fb_ptr;
    framebuffer_request.response = &test_fb_resp;

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_TRUE(info->fb_present);
    ASSERT_EQ(info->framebuffer.width, 1920);
    ASSERT_EQ(info->framebuffer.height, 1080);
    ASSERT_EQ(info->framebuffer.pitch, 7680);
    ASSERT_EQ(info->framebuffer.bpp, 32);
    ASSERT_EQ(info->framebuffer.red_mask_size, 8);
    ASSERT_EQ(info->framebuffer.red_mask_shift, 16);
    ASSERT_EQ(info->framebuffer.green_mask_size, 8);
    ASSERT_EQ(info->framebuffer.green_mask_shift, 8);
    ASSERT_EQ(info->framebuffer.blue_mask_size, 8);
    ASSERT_EQ(info->framebuffer.blue_mask_shift, 0);
    return 0;
}

TEST(framebuffer_zero_count) {
    reset_bootinfo_state();
    test_fb_resp.framebuffer_count = 0;
    test_fb_resp.framebuffers = NULL;
    framebuffer_request.response = &test_fb_resp;

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_FALSE(info->fb_present);
    return 0;
}

TEST(kernel_address_present) {
    reset_bootinfo_state();
    test_kaddr_resp.physical_base = 0x200000;
    test_kaddr_resp.virtual_base = 0xFFFFFFFF80000000ULL;
    kernel_address_request.response = &test_kaddr_resp;

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->kernel_phys_base, 0x200000);
    ASSERT_EQ(info->kernel_virt_base, 0xFFFFFFFF80000000ULL);
    return 0;
}

TEST(kernel_address_absent) {
    reset_bootinfo_state();
    kernel_address_request.response = NULL;

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->kernel_phys_base, 0);
    ASSERT_EQ(info->kernel_virt_base, 0);
    return 0;
}

TEST(rsdp_present) {
    reset_bootinfo_state();
    /* Simulate RSDP at virtual address hhdm_offset + 0xE0000 */
    uint64_t hhdm = 0xFFFF800000000000ULL;
    test_hhdm_resp.offset = hhdm;
    test_rsdp_resp.address = (void *)(uintptr_t)(hhdm + 0xE0000);
    rsdp_request.response = &test_rsdp_resp;

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->acpi_rsdp, 0xE0000);
    return 0;
}

TEST(rsdp_absent) {
    reset_bootinfo_state();
    rsdp_request.response = NULL;

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->acpi_rsdp, 0);
    return 0;
}

TEST(hhdm_offset_propagated) {
    reset_bootinfo_state();
    test_hhdm_resp.offset = 0xDEAD000000000000ULL;

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->hhdm_offset, 0xDEAD000000000000ULL);
    return 0;
}

TEST(memmap_single_entry) {
    reset_bootinfo_state();
    test_mmap_entries[0] = (struct limine_memmap_entry){
        .base = 0x100000, .length = 0x800000, .type = MEMMAP_KERNEL_AND_MODULES
    };
    setup_mmap_entries(1);

    const BootInfo *info = bootinfo_init();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->memory_map_count, 1);
    ASSERT_EQ(info->memory_map[0].type, MEMMAP_KERNEL_AND_MODULES);
    return 0;
}

/* --- Test suite export --- */

TestCase bootinfo_tests[] = {
    TEST_ENTRY(null_hhdm_returns_null),
    TEST_ENTRY(null_memmap_returns_null),
    TEST_ENTRY(valid_memmap),
    TEST_ENTRY(entry_count_clamped),
    TEST_ENTRY(framebuffer_absent),
    TEST_ENTRY(framebuffer_present),
    TEST_ENTRY(framebuffer_zero_count),
    TEST_ENTRY(kernel_address_present),
    TEST_ENTRY(kernel_address_absent),
    TEST_ENTRY(rsdp_present),
    TEST_ENTRY(rsdp_absent),
    TEST_ENTRY(hhdm_offset_propagated),
    TEST_ENTRY(memmap_single_entry),
};

int bootinfo_test_count = sizeof(bootinfo_tests) / sizeof(bootinfo_tests[0]);
