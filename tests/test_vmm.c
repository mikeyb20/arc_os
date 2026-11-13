/* arc_os — Host-side tests for kernel/mm/vmm.c page table logic */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_ARCH_X86_64_PAGING_H  /* Has inline asm (invlpg, mov cr3) */
#define ARCHOS_MM_PMM_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H             /* Use libc memset/memcpy */
#define ARCHOS_MM_VMM_H
#define ARCHOS_ARCH_HAL_H            /* stubbed below: TLB flush */

#include "proc/spinlock.h"   /* host build: simulated interrupt flag */

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }
#define KERNEL_PANIC() do { } while(0)

/* PTE constants (from paging.h) */
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_HUGE       (1ULL << 7)
#define PTE_PROTNONE   (1ULL << 9)
#define PTE_NX         (1ULL << 63)
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

#define PT_ENTRIES     512
#define PAGE_SIZE      4096
#define PAGE_ALIGN_UP(x)    (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL))
#define PAGE_ALIGN_DOWN(x)  ((x) & ~(PAGE_SIZE - 1ULL))

/* Index extraction macros */
#define PML4_INDEX(va)  (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va)  (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)    (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)    (((va) >> 12) & 0x1FF)

/* VMM flags (from vmm.h) */
#define VMM_FLAG_WRITABLE  (1 << 0)
#define VMM_FLAG_USER      (1 << 1)
#define VMM_FLAG_NOEXEC    (1 << 2)
#define VMM_FLAG_PROTNONE  (1 << 3)
typedef void (*vmm_defer_free_t)(uint64_t phys, int table, void *ctx);

/* User-space constants (from vmm.h) */
#define USER_STACK_TOP 0x00007FFFFFFFE000ULL

/* Errno (from vfs.h) */
#define ENOMEM 12

/* Memmap types (from bootinfo.h) */
#define MEMMAP_FRAMEBUFFER 7

/* Forward-declare vmm public API (since vmm.h is guarded) */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} MemoryMapEntry;

typedef struct {
    void    *address;
    uint64_t width, height, pitch;
    uint16_t bpp;
    uint8_t  red_mask_size, red_mask_shift;
    uint8_t  green_mask_size, green_mask_shift;
    uint8_t  blue_mask_size, blue_mask_shift;
} Framebuffer;

#define BOOTINFO_MAX_MEMMAP_ENTRIES 64

typedef struct {
    MemoryMapEntry memory_map[BOOTINFO_MAX_MEMMAP_ENTRIES];
    uint64_t       memory_map_count;
    Framebuffer    framebuffer;
    int            fb_present;
    uint64_t       acpi_rsdp;
    uint64_t       kernel_phys_base;
    uint64_t       kernel_virt_base;
    uint64_t       hhdm_offset;
} BootInfo;

void vmm_init(const BootInfo *info);
void vmm_map_page(uint64_t virt, uint64_t phys, uint32_t flags);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_get_phys(uint64_t virt);
uint64_t vmm_get_kernel_pml4(void);
uint64_t vmm_get_hhdm_offset(void);
int vmm_map_page_in(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t flags);
void vmm_unmap_page_in(uint64_t pml4, uint64_t virt);
uint64_t vmm_get_phys_in(uint64_t pml4, uint64_t virt);
int vmm_get_flags_in(uint64_t pml4, uint64_t virt);
int vmm_lookup_in(uint64_t pml4, uint64_t virt, uint64_t *phys);
uint64_t vmm_unmap_range_in(uint64_t pml4, uint64_t start, uint64_t end, vmm_defer_free_t defer, void *ctx);
void vmm_release_table(uint64_t phys);
void vmm_protect_range_in(uint64_t pml4, uint64_t start, uint64_t end, uint32_t flags);
int vmm_copy_range_in(uint64_t src, uint64_t dst, uint64_t start, uint64_t end, uint64_t *copied);
int vmm_move_range_in(uint64_t pml4, uint64_t from, uint64_t to, uint64_t len);
uint64_t vmm_create_user_pml4(void);
void vmm_free_user_pages(uint64_t pml4);
uint64_t vmm_user_table_pages(void);

/* Static arena for pmm_alloc_page stub — enough for page tables.
 * With hhdm_offset=0, phys_to_virt(addr)==addr, so we need the arena
 * addresses to work as both physical and virtual pointers. */
#define ARENA_PAGES 256
static uint8_t arena[ARENA_PAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static int arena_next;

static int arena_limit = ARENA_PAGES;   /* lower it to make allocations fail */
static int free_count;

static uint64_t pmm_alloc_page(void) {
    if (arena_next >= arena_limit) return 0;
    uint64_t addr = (uint64_t)&arena[arena_next * PAGE_SIZE];
    arena_next++;
    return addr;
}

static void pmm_free_page(uint64_t phys_addr) {
    (void)phys_addr;
    free_count++;
}

/* hal.h stub: the kernel-range TLB flush vmm_unmap_page does */
static int tlb_kernel_flushes;
static void hal_tlb_flush_kernel(uint64_t start, uint64_t end) {
    (void)start;
    (void)end;
    tlb_kernel_flushes++;
}

/* Tracking stubs for paging operations */
static int invlpg_call_count;
static uint64_t invlpg_last_addr;
static int write_cr3_call_count;
static uint64_t write_cr3_last_value;

static void paging_invlpg(uint64_t vaddr) {
    invlpg_call_count++;
    invlpg_last_addr = vaddr;
}

static void paging_write_cr3(uint64_t cr3) {
    write_cr3_call_count++;
    write_cr3_last_value = cr3;
}

/* Linker symbols */
static char _kernel_start[1];
static char _kernel_end[1];

/* Include the real vmm.c */
#include "../kernel/mm/vmm.c"

static void reset_vmm_state(void) {
    memset(arena, 0, sizeof(arena));
    arena_next = 0;
    kernel_pml4_phys = 0;
    hhdm_offset = 0;  /* phys == virt for testing */
    invlpg_call_count = 0;
    invlpg_last_addr = 0;
    write_cr3_call_count = 0;
    write_cr3_last_value = 0;
    arena_limit = ARENA_PAGES;
    free_count = 0;
    tlb_kernel_flushes = 0;
    user_table_pages = 0;

    /* Allocate a PML4 manually */
    kernel_pml4_phys = pmm_alloc_page();
}

/* --- Tests --- */

TEST(map_then_get_phys) {
    reset_vmm_state();
    uint64_t virt = 0x1000000;  /* 16 MB */
    uint64_t phys = 0x2000000;  /* 32 MB — fake phys, only checked in PTE */
    vmm_map_page(virt, phys, VMM_FLAG_WRITABLE);

    /* vmm_get_phys walks the page tables and returns phys + page_offset */
    uint64_t result = vmm_get_phys(virt);
    ASSERT_EQ(result, phys);
    return 0;
}

TEST(get_phys_unmapped_returns_zero) {
    reset_vmm_state();
    ASSERT_EQ(vmm_get_phys(0x5000000), 0);
    return 0;
}

TEST(map_multiple_pages) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    vmm_map_page(0x2000000, 0xB000, VMM_FLAG_WRITABLE);
    vmm_map_page(0x3000000, 0xC000, VMM_FLAG_WRITABLE);

    ASSERT_EQ(vmm_get_phys(0x1000000), 0xA000);
    ASSERT_EQ(vmm_get_phys(0x2000000), 0xB000);
    ASSERT_EQ(vmm_get_phys(0x3000000), 0xC000);
    return 0;
}

TEST(map_overwrites_existing) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    ASSERT_EQ(vmm_get_phys(0x1000000), 0xA000);

    /* Re-map to different physical */
    vmm_map_page(0x1000000, 0xB000, VMM_FLAG_WRITABLE);
    ASSERT_EQ(vmm_get_phys(0x1000000), 0xB000);
    return 0;
}

TEST(unmap_page_clears_mapping) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    vmm_unmap_page(0x1000000);
    ASSERT_EQ(vmm_get_phys(0x1000000), 0);
    return 0;
}

TEST(unmap_calls_invlpg) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    invlpg_call_count = 0;
    vmm_unmap_page(0x1000000);
    ASSERT_EQ(invlpg_call_count, 1);
    ASSERT_EQ(invlpg_last_addr, 0x1000000);
    return 0;
}

TEST(unmap_nonexistent_is_noop) {
    reset_vmm_state();
    invlpg_call_count = 0;
    vmm_unmap_page(0x5000000);  /* Never mapped */
    ASSERT_EQ(invlpg_call_count, 0);
    return 0;
}

TEST(unmap_does_not_affect_others) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    vmm_map_page(0x1001000, 0xB000, VMM_FLAG_WRITABLE);
    vmm_unmap_page(0x1000000);
    ASSERT_EQ(vmm_get_phys(0x1000000), 0);
    ASSERT_EQ(vmm_get_phys(0x1001000), 0xB000);
    return 0;
}

TEST(flags_writable) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);

    /* Walk to PTE manually */
    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t pte = pt[PT_INDEX(0x1000000ULL)];

    ASSERT_TRUE(pte & PTE_PRESENT);
    ASSERT_TRUE(pte & PTE_WRITABLE);
    ASSERT_FALSE(pte & PTE_USER);
    ASSERT_FALSE(pte & PTE_NX);
    return 0;
}

TEST(flags_user) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_USER);

    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t pte = pt[PT_INDEX(0x1000000ULL)];

    ASSERT_TRUE(pte & PTE_PRESENT);
    ASSERT_TRUE(pte & PTE_USER);
    ASSERT_FALSE(pte & PTE_WRITABLE);
    return 0;
}

TEST(flags_noexec) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_NOEXEC);

    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t pte = pt[PT_INDEX(0x1000000ULL)];

    ASSERT_TRUE(pte & PTE_PRESENT);
    ASSERT_TRUE(pte & PTE_NX);
    return 0;
}

TEST(flags_none) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, 0);

    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(0x1000000ULL)] & PTE_ADDR_MASK);
    uint64_t pte = pt[PT_INDEX(0x1000000ULL)];

    /* Only PTE_PRESENT, no other flags */
    ASSERT_TRUE(pte & PTE_PRESENT);
    ASSERT_FALSE(pte & PTE_WRITABLE);
    ASSERT_FALSE(pte & PTE_USER);
    ASSERT_FALSE(pte & PTE_NX);
    return 0;
}

TEST(intermediate_tables_permissive) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, 0);

    /* Intermediate entries (PML4, PDPT, PD) should have P+W+U */
    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    uint64_t pml4e = pml4[PML4_INDEX(0x1000000ULL)];
    ASSERT_TRUE(pml4e & PTE_PRESENT);
    ASSERT_TRUE(pml4e & PTE_WRITABLE);
    ASSERT_TRUE(pml4e & PTE_USER);

    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4e & PTE_ADDR_MASK);
    uint64_t pdpte = pdpt[PDPT_INDEX(0x1000000ULL)];
    ASSERT_TRUE(pdpte & PTE_PRESENT);
    ASSERT_TRUE(pdpte & PTE_WRITABLE);
    ASSERT_TRUE(pdpte & PTE_USER);

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpte & PTE_ADDR_MASK);
    uint64_t pde = pd[PD_INDEX(0x1000000ULL)];
    ASSERT_TRUE(pde & PTE_PRESENT);
    ASSERT_TRUE(pde & PTE_WRITABLE);
    ASSERT_TRUE(pde & PTE_USER);
    return 0;
}

TEST(same_pt_reused) {
    reset_vmm_state();
    /* Two pages in the same PT (adjacent pages) */
    int before = arena_next;
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    int after_first = arena_next;
    vmm_map_page(0x1001000, 0xB000, VMM_FLAG_WRITABLE);
    int after_second = arena_next;

    /* First mapping allocates 3 tables (PDPT, PD, PT).
     * Second mapping in same PT allocates no new tables. */
    ASSERT_EQ(after_first - before, 3);
    ASSERT_EQ(after_second - after_first, 0);
    return 0;
}

TEST(different_pml4_entries) {
    reset_vmm_state();
    /* Addresses that differ at PML4 index level */
    uint64_t v1 = 0ULL;                       /* PML4 idx 0 */
    uint64_t v2 = (1ULL << 39);               /* PML4 idx 1 */
    vmm_map_page(v1, 0xA000, VMM_FLAG_WRITABLE);
    vmm_map_page(v2, 0xB000, VMM_FLAG_WRITABLE);

    ASSERT_EQ(vmm_get_phys(v1), 0xA000);
    ASSERT_EQ(vmm_get_phys(v2), 0xB000);
    return 0;
}

TEST(get_phys_preserves_offset) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    /* virt+0x42 → phys+0x42 */
    ASSERT_EQ(vmm_get_phys(0x1000042), 0xA042);
    return 0;
}

TEST(get_phys_last_byte_of_page) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    /* virt+0xFFF → phys+0xFFF */
    ASSERT_EQ(vmm_get_phys(0x1000FFF), 0xAFFF);
    return 0;
}

TEST(get_kernel_pml4) {
    reset_vmm_state();
    uint64_t pml4 = vmm_get_kernel_pml4();
    ASSERT_NEQ(pml4, 0);
    ASSERT_EQ(pml4, kernel_pml4_phys);
    return 0;
}

TEST(get_hhdm_offset) {
    reset_vmm_state();
    ASSERT_EQ(vmm_get_hhdm_offset(), 0);
    return 0;
}

TEST(init_sets_pml4) {
    /* Reset fully (including kernel_pml4_phys) */
    memset(arena, 0, sizeof(arena));
    arena_next = 0;
    kernel_pml4_phys = 0;
    hhdm_offset = 0;
    write_cr3_call_count = 0;

    /* Build a minimal BootInfo */
    BootInfo info;
    memset(&info, 0, sizeof(info));
    info.hhdm_offset = 0;
    info.memory_map[0].base = 0;
    info.memory_map[0].length = 0x200000;  /* 2 MB */
    info.memory_map[0].type = 0;  /* USABLE */
    info.memory_map_count = 1;
    info.kernel_phys_base = (uint64_t)_kernel_start;
    info.fb_present = 0;

    vmm_init(&info);

    ASSERT_NEQ(kernel_pml4_phys, 0);
    ASSERT_EQ(write_cr3_call_count, 1);
    ASSERT_EQ(write_cr3_last_value, kernel_pml4_phys);
    return 0;
}

/* --- M1-07: user range walkers --- */

/* Frames an unmap handed back */
typedef struct {
    int pages, tables;
    uint64_t last_page;
} Deferred;

static void defer_cb(uint64_t phys, int table, void *ctx) {
    Deferred *d = ctx;
    if (table) {
        d->tables++;
    } else {
        d->pages++;
        d->last_page = phys;
    }
}

/* A fresh user PML4 (vmm_create_user_pml4 needs the kernel PML4 to copy) */
static uint64_t new_user_pml4(void) {
    uint64_t pml4 = vmm_create_user_pml4();
    return pml4;
}

TEST(unmap_range_frees_emptied_tables) {
    reset_vmm_state();
    uint64_t pml4 = new_user_pml4();
    ASSERT_EQ(vmm_user_table_pages(), 1);   /* the PML4 */
    ASSERT_EQ(vmm_map_page_in(pml4, 0x400000, 0xA000, VMM_FLAG_USER), 0);
    ASSERT_EQ(vmm_map_page_in(pml4, 0x401000, 0xB000, VMM_FLAG_USER), 0);
    ASSERT_EQ(vmm_user_table_pages(), 4);   /* + PDPT, PD, PT */
    Deferred d = {0, 0, 0};
    /* Half the range: the PT still holds a page, nothing is freed */
    ASSERT_EQ(vmm_unmap_range_in(pml4, 0x400000, 0x401000, defer_cb, &d), 1);
    ASSERT_EQ(d.pages, 1);
    ASSERT_EQ(d.last_page, 0xA000);
    ASSERT_EQ(d.tables, 0);
    ASSERT_EQ(vmm_lookup_in(pml4, 0x400000, NULL), 0);
    ASSERT_EQ(vmm_lookup_in(pml4, 0x401000, NULL), 1);
    /* The rest: PT, PD and PDPT are all empty now */
    ASSERT_EQ(vmm_unmap_range_in(pml4, 0x400000, 0x800000, defer_cb, &d), 1);
    ASSERT_EQ(d.tables, 3);
    for (int i = 0; i < 3; i++) vmm_release_table(0);
    ASSERT_EQ(vmm_user_table_pages(), 1);
    uint64_t *top = (uint64_t *)pml4;
    ASSERT_EQ(top[PML4_INDEX(0x400000)], 0);
    return 0;
}

TEST(unmap_range_skips_missing_tables) {
    reset_vmm_state();
    uint64_t pml4 = new_user_pml4();
    ASSERT_EQ(vmm_map_page_in(pml4, 0x7FFF00000000ULL, 0xA000, VMM_FLAG_USER), 0);
    Deferred d = {0, 0, 0};
    /* 128 TiB walk with one page in it: one step per missing table span */
    ASSERT_EQ(vmm_unmap_range_in(pml4, 0x10000, 0x7FFFFFFFF000ULL, defer_cb, &d), 1);
    ASSERT_EQ(d.pages, 1);
    ASSERT_EQ(d.tables, 3);
    return 0;
}

TEST(protnone_keeps_frame) {
    reset_vmm_state();
    uint64_t pml4 = new_user_pml4();
    ASSERT_EQ(vmm_map_page_in(pml4, 0x400000, 0xA000, VMM_FLAG_USER | VMM_FLAG_WRITABLE), 0);
    vmm_protect_range_in(pml4, 0x400000, 0x401000, VMM_FLAG_PROTNONE);
    ASSERT_EQ(vmm_get_phys_in(pml4, 0x400000), 0);   /* not present */
    ASSERT_EQ(vmm_get_flags_in(pml4, 0x400000), VMM_FLAG_PROTNONE);
    uint64_t phys = 0;
    ASSERT_EQ(vmm_lookup_in(pml4, 0x400000, &phys), 1);
    ASSERT_EQ(phys, 0xA000);
    vmm_protect_range_in(pml4, 0x400000, 0x401000, VMM_FLAG_USER);
    ASSERT_EQ(vmm_get_phys_in(pml4, 0x400000), 0xA000);
    ASSERT_EQ(vmm_get_flags_in(pml4, 0x400000), VMM_FLAG_USER);
    /* protect leaves unmapped pages unmapped */
    vmm_protect_range_in(pml4, 0x401000, 0x402000, VMM_FLAG_USER);
    ASSERT_EQ(vmm_lookup_in(pml4, 0x401000, NULL), 0);
    return 0;
}

TEST(protnone_page_is_unmapped_and_freed) {
    reset_vmm_state();
    uint64_t pml4 = new_user_pml4();
    ASSERT_EQ(vmm_map_page_in(pml4, 0x400000, 0xA000, VMM_FLAG_PROTNONE), 0);
    Deferred d = {0, 0, 0};
    ASSERT_EQ(vmm_unmap_range_in(pml4, 0x400000, 0x401000, defer_cb, &d), 1);
    ASSERT_EQ(d.last_page, 0xA000);
    return 0;
}

TEST(copy_range_copies_contents_and_flags) {
    reset_vmm_state();
    uint64_t src = new_user_pml4();
    uint64_t dst = new_user_pml4();
    uint64_t page = pmm_alloc_page();
    memset((void *)page, 0x5A, PAGE_SIZE);
    ASSERT_EQ(vmm_map_page_in(src, 0x400000, page, VMM_FLAG_USER | VMM_FLAG_WRITABLE), 0);
    ASSERT_EQ(vmm_map_page_in(src, 0x402000, page, VMM_FLAG_PROTNONE), 0);
    uint64_t copied = 0;
    ASSERT_EQ(vmm_copy_range_in(src, dst, 0x400000, 0x403000, &copied), 0);
    ASSERT_EQ(copied, 2);
    uint64_t cp = vmm_get_phys_in(dst, 0x400000);
    ASSERT_NEQ(cp, 0);
    ASSERT_NEQ(cp, page);   /* a copy, not shared (copy-on-write is M1-08) */
    ASSERT_EQ(((uint8_t *)cp)[100], 0x5A);
    ASSERT_EQ(vmm_get_flags_in(dst, 0x400000), VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    ASSERT_EQ(vmm_get_flags_in(dst, 0x402000), VMM_FLAG_PROTNONE);
    ASSERT_EQ(vmm_lookup_in(dst, 0x401000, NULL), 0);
    return 0;
}

TEST(copy_range_reports_oom) {
    reset_vmm_state();
    uint64_t src = new_user_pml4();
    uint64_t dst = new_user_pml4();
    ASSERT_EQ(vmm_map_page_in(src, 0x400000, 0xA000, VMM_FLAG_USER), 0);
    arena_limit = arena_next;   /* no page for the copy */
    uint64_t copied = 0;
    ASSERT_EQ(vmm_copy_range_in(src, dst, 0x400000, 0x401000, &copied), -ENOMEM);
    ASSERT_EQ(copied, 0);
    return 0;
}

TEST(move_range_moves_entries) {
    reset_vmm_state();
    uint64_t pml4 = new_user_pml4();
    ASSERT_EQ(vmm_map_page_in(pml4, 0x400000, 0xA000, VMM_FLAG_USER), 0);
    ASSERT_EQ(vmm_map_page_in(pml4, 0x402000, 0xC000, VMM_FLAG_USER | VMM_FLAG_WRITABLE), 0);
    ASSERT_EQ(vmm_move_range_in(pml4, 0x400000, 0x10000000, 0x3000), 0);
    ASSERT_EQ(vmm_lookup_in(pml4, 0x400000, NULL), 0);
    ASSERT_EQ(vmm_lookup_in(pml4, 0x402000, NULL), 0);
    ASSERT_EQ(vmm_get_phys_in(pml4, 0x10000000), 0xA000);
    ASSERT_EQ(vmm_lookup_in(pml4, 0x10001000, NULL), 0);
    ASSERT_EQ(vmm_get_phys_in(pml4, 0x10002000), 0xC000);
    ASSERT_EQ(vmm_get_flags_in(pml4, 0x10002000), VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    return 0;
}

TEST(move_range_oom_leaves_source) {
    reset_vmm_state();
    uint64_t pml4 = new_user_pml4();
    ASSERT_EQ(vmm_map_page_in(pml4, 0x400000, 0xA000, VMM_FLAG_USER), 0);
    arena_limit = arena_next;   /* no table for the destination */
    ASSERT_EQ(vmm_move_range_in(pml4, 0x400000, 0x7F0000000000ULL, 0x1000), -ENOMEM);
    ASSERT_EQ(vmm_get_phys_in(pml4, 0x400000), 0xA000);
    return 0;
}

TEST(map_page_in_reports_oom) {
    reset_vmm_state();
    uint64_t pml4 = new_user_pml4();
    arena_limit = arena_next;
    ASSERT_EQ(vmm_map_page_in(pml4, 0x400000, 0xA000, VMM_FLAG_USER), -ENOMEM);
    return 0;
}

TEST(create_user_pml4_copies_kernel_half) {
    reset_vmm_state();
    uint64_t *kern = (uint64_t *)kernel_pml4_phys;
    kern[300] = 0x12345003;
    uint64_t pml4 = new_user_pml4();
    ASSERT_EQ(((uint64_t *)pml4)[300], 0x12345003);
    ASSERT_EQ(((uint64_t *)pml4)[0], 0);
    arena_limit = arena_next;
    ASSERT_EQ(vmm_create_user_pml4(), 0);   /* out of memory: 0, no crash */
    return 0;
}

TEST(free_user_pages_frees_everything) {
    reset_vmm_state();
    uint64_t pml4 = new_user_pml4();
    ASSERT_EQ(vmm_map_page_in(pml4, 0x400000, 0xA000, VMM_FLAG_USER), 0);
    ASSERT_EQ(vmm_map_page_in(pml4, 0x7FFF00000000ULL, 0xB000, VMM_FLAG_PROTNONE), 0);
    free_count = 0;
    vmm_free_user_pages(pml4);
    ASSERT_EQ(free_count, 2 + 6 + 1);   /* 2 pages, 2 x (PDPT, PD, PT), the PML4 */
    ASSERT_EQ(vmm_user_table_pages(), 0);
    return 0;
}

TEST(kernel_unmap_flushes_all_cpus) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    vmm_unmap_page(0x1000000);
    ASSERT_EQ(tlb_kernel_flushes, 1);
    return 0;
}

TEST(get_flags_in_round_trips) {
    reset_vmm_state();
    uint64_t pml4 = kernel_pml4_phys;
    vmm_map_page_in(pml4, 0x400000, 0xA000, VMM_FLAG_USER | VMM_FLAG_NOEXEC);
    ASSERT_EQ(vmm_get_flags_in(pml4, 0x400000), VMM_FLAG_USER | VMM_FLAG_NOEXEC);
    ASSERT_EQ(vmm_get_flags_in(pml4, 0x401000), -1);
    return 0;
}

TEST(remap_after_unmap) {
    reset_vmm_state();
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    vmm_unmap_page(0x1000000);
    ASSERT_EQ(vmm_get_phys(0x1000000), 0);

    vmm_map_page(0x1000000, 0xB000, VMM_FLAG_WRITABLE);
    ASSERT_EQ(vmm_get_phys(0x1000000), 0xB000);
    return 0;
}

TEST(ensure_table_creates_on_first_use) {
    reset_vmm_state();
    int before = arena_next;
    vmm_map_page(0x1000000, 0xA000, VMM_FLAG_WRITABLE);
    int after = arena_next;
    /* First mapping allocates 3 intermediate tables: PDPT, PD, PT */
    ASSERT_EQ(after - before, 3);
    return 0;
}

/* --- Test suite export --- */

TestCase vmm_tests[] = {
    TEST_ENTRY(map_then_get_phys),
    TEST_ENTRY(get_phys_unmapped_returns_zero),
    TEST_ENTRY(map_multiple_pages),
    TEST_ENTRY(map_overwrites_existing),
    TEST_ENTRY(unmap_page_clears_mapping),
    TEST_ENTRY(unmap_calls_invlpg),
    TEST_ENTRY(unmap_nonexistent_is_noop),
    TEST_ENTRY(unmap_does_not_affect_others),
    TEST_ENTRY(flags_writable),
    TEST_ENTRY(flags_user),
    TEST_ENTRY(flags_noexec),
    TEST_ENTRY(flags_none),
    TEST_ENTRY(intermediate_tables_permissive),
    TEST_ENTRY(same_pt_reused),
    TEST_ENTRY(different_pml4_entries),
    TEST_ENTRY(get_phys_preserves_offset),
    TEST_ENTRY(get_phys_last_byte_of_page),
    TEST_ENTRY(unmap_range_frees_emptied_tables),
    TEST_ENTRY(unmap_range_skips_missing_tables),
    TEST_ENTRY(protnone_keeps_frame),
    TEST_ENTRY(protnone_page_is_unmapped_and_freed),
    TEST_ENTRY(copy_range_copies_contents_and_flags),
    TEST_ENTRY(copy_range_reports_oom),
    TEST_ENTRY(move_range_moves_entries),
    TEST_ENTRY(move_range_oom_leaves_source),
    TEST_ENTRY(map_page_in_reports_oom),
    TEST_ENTRY(create_user_pml4_copies_kernel_half),
    TEST_ENTRY(free_user_pages_frees_everything),
    TEST_ENTRY(kernel_unmap_flushes_all_cpus),
    TEST_ENTRY(get_flags_in_round_trips),
    TEST_ENTRY(get_kernel_pml4),
    TEST_ENTRY(get_hhdm_offset),
    TEST_ENTRY(init_sets_pml4),
    TEST_ENTRY(remap_after_unmap),
    TEST_ENTRY(ensure_table_creates_on_first_use),
};

int vmm_test_count = sizeof(vmm_tests) / sizeof(vmm_tests[0]);
