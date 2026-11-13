/* arc_os — Host-side tests for kernel/drivers/dma.c (DMA helpers, M1-09)
 *
 * The real dma.c runs against a fake page allocator (it records the order and
 * flags asked for and hands out a chosen physical address) and a fake page
 * walk (vmm_get_phys), so mask checks, zone choice and contiguity are
 * checked without memory of the sizes involved. */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ARCHOS_MM_PMM_H
#define ARCHOS_MM_VMM_H
#define ARCHOS_LIB_KPRINTF_H

#include "mm/gfp.h"

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}

/* The real mm/page.h (PAGE_SIZE, page_order_for); its page/address
 * conversions are renamed away for the fakes below */
#define page_to_phys page_h_page_to_phys
#define page_to_virt page_h_page_to_virt
#define virt_to_page page_h_virt_to_page
#include "mm/page.h"
#undef page_to_phys
#undef page_to_virt
#undef virt_to_page
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1ULL))

static struct page fake_page;
static uint64_t fake_phys;   /* the physical address fake_page stands for */
static uint8_t fake_virt[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static int last_order = -1;
static uint32_t last_gfp;
static int alloc_calls, free_calls;
static int alloc_fails;

static struct page *alloc_pages(unsigned order, uint32_t flags) {
    alloc_calls++;
    last_order = (int)order;
    last_gfp = flags;
    return alloc_fails ? NULL : &fake_page;
}

static void free_pages(struct page *pg, unsigned order) {
    (void)pg;
    free_calls++;
    last_order = (int)order;
}

static uint64_t page_to_phys(struct page *pg) {
    (void)pg;
    return fake_phys;
}

static void *page_to_virt(struct page *pg) {
    (void)pg;
    return fake_virt;
}

static struct page *virt_to_page(void *p) {
    (void)p;
    return &fake_page;
}

/* Fake kernel page walk: virtual page v maps to phys_of[v - VBASE pages] */
#define VBASE 0x100000000ULL
static uint64_t phys_of[4];

static uint64_t vmm_get_phys(uint64_t virt) {
    uint64_t i = (virt - VBASE) / PAGE_SIZE;
    if (virt < VBASE || i >= 4 || phys_of[i] == 0) return 0;
    return phys_of[i] + (virt & (PAGE_SIZE - 1));
}

#include "../kernel/drivers/dma.c"

static void reset(Device *dev, uint64_t page_phys) {
    memset(dev, 0, sizeof(*dev));
    dev->name = "dev";
    dev->dma_mask = ~0ULL;
    fake_phys = page_phys;
    last_order = -1;
    last_gfp = 0;
    alloc_calls = free_calls = alloc_fails = 0;
    memset(phys_of, 0, sizeof(phys_of));
}

static int test_dma_page_order_for(void) {
    ASSERT_EQ(page_order_for(1), 0);
    ASSERT_EQ(page_order_for(4096), 0);
    ASSERT_EQ(page_order_for(4097), 1);
    ASSERT_EQ(page_order_for(3 * 4096), 2);
    ASSERT_EQ(page_order_for((size_t)4096 << 10), 10);
    ASSERT_EQ(page_order_for(((size_t)4096 << 10) + 1), 11);
    return 0;
}

static int test_dma_fits(void) {
    ASSERT_TRUE(dma_fits(0x1000, 0x1000, DMA_BIT_MASK(32)));
    ASSERT_TRUE(dma_fits(0xFFFFF000, 0x1000, DMA_BIT_MASK(32)));
    ASSERT_FALSE(dma_fits(0xFFFFF000, 0x1001, DMA_BIT_MASK(32)));
    ASSERT_FALSE(dma_fits(0x100000000ULL, 1, DMA_BIT_MASK(32)));
    ASSERT_FALSE(dma_fits(0x1000, 0, DMA_BIT_MASK(32)));               /* empty */
    ASSERT_FALSE(dma_fits(~0ULL - 10, 100, DMA_BIT_MASK(64)));         /* wraps */
    ASSERT_TRUE(dma_fits(0x0FFFF000, 0x1000, DMA_BIT_MASK(28)));
    ASSERT_TRUE(DMA_BIT_MASK(64) == ~0ULL);
    ASSERT_TRUE(DMA_BIT_MASK(32) == 0xFFFFFFFFULL);
    return 0;
}

static int test_dma_set_mask(void) {
    Device d;
    reset(&d, 0x1000);
    ASSERT_EQ(dma_set_mask(&d, DMA_BIT_MASK(32)), 0);
    ASSERT_TRUE(d.dma_mask == 0xFFFFFFFFULL);
    ASSERT_EQ(dma_set_mask(&d, 0xFF), -EINVAL);
    ASSERT_TRUE(d.dma_mask == 0xFFFFFFFFULL);
    return 0;
}

/* Size rounds up to a buddy order; zeroed; DMA32 when the mask or flag asks */
static int test_dma_coherent_order_and_zone(void) {
    Device d;
    uint64_t bus = 0;
    reset(&d, 0x200000);
    ASSERT_TRUE(dma_alloc_coherent(&d, 100, &bus, 0) == fake_virt);
    ASSERT_EQ(last_order, 0);
    ASSERT_TRUE(last_gfp & GFP_ZERO);
    ASSERT_FALSE(last_gfp & GFP_DMA32);
    ASSERT_EQ(bus, 0x200000);
    dma_alloc_coherent(&d, 4097, &bus, 0);
    ASSERT_EQ(last_order, 1);
    dma_alloc_coherent(&d, 0x6000, &bus, DMA_BELOW_4G);   /* 24 KiB -> 32 KiB */
    ASSERT_EQ(last_order, 3);
    ASSERT_TRUE(last_gfp & GFP_DMA32);
    dma_alloc_coherent(&d, (size_t)PAGE_SIZE << PAGE_MAX_ORDER, &bus, 0);
    ASSERT_EQ(last_order, PAGE_MAX_ORDER);
    int calls = alloc_calls;
    ASSERT_TRUE(dma_alloc_coherent(&d, ((size_t)PAGE_SIZE << PAGE_MAX_ORDER) + 1, &bus, 0) == NULL);
    ASSERT_TRUE(dma_alloc_coherent(&d, 0, &bus, 0) == NULL);
    ASSERT_EQ(alloc_calls, calls);   /* refused before the allocator */
    dma_set_mask(&d, DMA_BIT_MASK(32));
    dma_alloc_coherent(&d, 100, &bus, 0);
    ASSERT_TRUE(last_gfp & GFP_DMA32);
    alloc_fails = 1;
    ASSERT_TRUE(dma_alloc_coherent(&d, 100, &bus, 0) == NULL);
    return 0;
}

/* A block the device can't reach is given back (no bounce buffers) */
static int test_dma_coherent_above_mask(void) {
    Device d;
    uint64_t bus = 0;
    reset(&d, 0x0FFFF000);   /* fits 28 bits for one page only */
    dma_set_mask(&d, DMA_BIT_MASK(28));
    ASSERT_TRUE(dma_alloc_coherent(&d, 4096, &bus, 0) != NULL);
    ASSERT_TRUE(dma_alloc_coherent(&d, 8192, &bus, 0) == NULL);
    ASSERT_EQ(free_calls, 1);
    ASSERT_EQ(last_order, 1);
    dma_free_coherent(&d, 4096, fake_virt, 0x0FFFF000);
    ASSERT_EQ(free_calls, 2);
    ASSERT_EQ(last_order, 0);
    return 0;
}

/* Streaming maps: identity, contiguity checked page by page, mask checked */
static int test_dma_map_single(void) {
    Device d;
    reset(&d, 0);
    phys_of[0] = 0x5000;
    phys_of[1] = 0x6000;
    phys_of[2] = 0x9000;   /* not contiguous with page 1 */
    const void *p = (const void *)(uintptr_t)(VBASE + 0x800);
    ASSERT_TRUE(dma_map_single(&d, p, 0x100, DMA_TO_DEVICE) == 0x5800);
    ASSERT_TRUE(dma_map_single(&d, p, 0x1800, DMA_TO_DEVICE) == 0x5800);   /* ends in page 1 */
    ASSERT_TRUE(dma_map_single(&d, p, 0x1801, DMA_TO_DEVICE) == DMA_MAPPING_ERROR);   /* reaches page 2 */
    ASSERT_TRUE(dma_map_single(&d, p, 0, DMA_TO_DEVICE) == DMA_MAPPING_ERROR);
    ASSERT_TRUE(dma_map_single(&d, (const void *)(uintptr_t)(VBASE + 3 * PAGE_SIZE), 16, DMA_FROM_DEVICE) ==
                DMA_MAPPING_ERROR);   /* not mapped */
    dma_set_mask(&d, 0x57FF);
    ASSERT_TRUE(dma_map_single(&d, p, 0x10, DMA_TO_DEVICE) == DMA_MAPPING_ERROR);
    return 0;
}

static int test_dma_map_page(void) {
    Device d;
    reset(&d, 0xFFFFF000);
    dma_set_mask(&d, DMA_BIT_MASK(32));
    ASSERT_TRUE(dma_map_page(&d, &fake_page, 0x10, 0x100, DMA_BIDIRECTIONAL) == 0xFFFFF010);
    ASSERT_TRUE(dma_map_page(&d, &fake_page, 0x10, 0x1000, DMA_BIDIRECTIONAL) == DMA_MAPPING_ERROR);
    dma_unmap(&d, 0xFFFFF010, 0x100, DMA_BIDIRECTIONAL);   /* no-op without an IOMMU */
    return 0;
}

TestCase dma_tests[] = {
    {"dma_page_order_for", test_dma_page_order_for},
    {"dma_fits", test_dma_fits},
    {"dma_set_mask", test_dma_set_mask},
    {"dma_coherent_order_and_zone", test_dma_coherent_order_and_zone},
    {"dma_coherent_above_mask", test_dma_coherent_above_mask},
    {"dma_map_single", test_dma_map_single},
    {"dma_map_page", test_dma_map_page},
};

int dma_test_count = sizeof(dma_tests) / sizeof(dma_tests[0]);
