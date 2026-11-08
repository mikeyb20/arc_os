/* arc_os — Host-side tests for ELF64 loader */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Guard kernel headers — we provide stubs (elf.h itself is used as is) */
#define ARCHOS_MM_VMM_H
#define ARCHOS_MM_PMM_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_FS_VFS_H

#include "../kernel/proc/elf.h"

/* Constants elf.c needs */
#define PAGE_SIZE 4096
#define PAGE_ALIGN_UP(x)    (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL))
#define PAGE_ALIGN_DOWN(x)  ((x) & ~(PAGE_SIZE - 1ULL))
#define VMM_FLAG_WRITABLE  (1 << 0)
#define VMM_FLAG_USER      (1 << 1)
#define VMM_FLAG_NOEXEC    (1 << 2)
#define EINVAL 22
#define ENOMEM 12
#define ENOEXEC 8

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Track vmm_map_page_in calls, and the resulting mappings */
#define MAX_MAP_CALLS 64
static struct {
    uint64_t pml4;
    uint64_t virt;
    uint64_t phys;
    uint32_t flags;
} map_calls[MAX_MAP_CALLS];
static int map_call_count = 0;

static struct {
    uint64_t virt;
    uint64_t phys;
    uint32_t flags;
} mappings[MAX_MAP_CALLS];
static int mapping_count = 0;

/* Stub PMM: allocate real heap memory and return its address as "physical" */
#define MAX_PAGES 32
static void *allocated_pages[MAX_PAGES];
static int allocated_page_count = 0;

static uint64_t pmm_alloc_page(void) {
    if (allocated_page_count >= MAX_PAGES) return 0;
    void *p = aligned_alloc(PAGE_SIZE, PAGE_SIZE);   /* page-aligned, like real frames */
    if (!p) return 0;
    memset(p, 0, PAGE_SIZE);
    allocated_pages[allocated_page_count++] = p;
    return (uint64_t)(uintptr_t)p;
}

/* Stub VMM: hhdm offset is 0 so phys == virt (the "phys" is already a host pointer) */
static uint64_t vmm_get_hhdm_offset(void) { return 0; }

static int mapping_find(uint64_t virt) {
    for (int i = 0; i < mapping_count; i++) {
        if (mappings[i].virt == virt) return i;
    }
    return -1;
}

static void vmm_map_page_in(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t flags) {
    if (map_call_count < MAX_MAP_CALLS) {
        map_calls[map_call_count].pml4 = pml4;
        map_calls[map_call_count].virt = virt;
        map_calls[map_call_count].phys = phys;
        map_calls[map_call_count].flags = flags;
        map_call_count++;
    }
    int i = mapping_find(virt);
    if (i < 0 && mapping_count < MAX_MAP_CALLS) i = mapping_count++;
    if (i >= 0) {
        mappings[i].virt = virt;
        mappings[i].phys = phys;
        mappings[i].flags = flags;
    }
}

static int vmm_get_flags_in(uint64_t pml4, uint64_t virt) {
    (void)pml4;
    int i = mapping_find(virt);
    return i < 0 ? -1 : (int)mappings[i].flags;
}

static uint64_t vmm_get_phys_in(uint64_t pml4, uint64_t virt) {
    (void)pml4;
    int i = mapping_find(PAGE_ALIGN_DOWN(virt));
    return i < 0 ? 0 : mappings[i].phys + (virt & (PAGE_SIZE - 1));
}

/* Reset test state */
static void reset_stubs(void) {
    map_call_count = 0;
    memset(map_calls, 0, sizeof(map_calls));
    mapping_count = 0;
    memset(mappings, 0, sizeof(mappings));
    /* Free allocated pages */
    for (int i = 0; i < allocated_page_count; i++) {
        free(allocated_pages[i]);
        allocated_pages[i] = NULL;
    }
    allocated_page_count = 0;
}

/* Include the real elf.c implementation */
#include "../kernel/proc/elf.c"

/* Helper: build a minimal valid ELF64 executable with one PT_LOAD segment */
static void build_minimal_elf(uint8_t *buf, size_t buf_size,
                              uint64_t vaddr, uint64_t entry,
                              uint32_t p_flags, uint64_t filesz, uint64_t memsz) {
    memset(buf, 0, buf_size);

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    ehdr->e_ident[0] = 0x7F;
    ehdr->e_ident[1] = 'E';
    ehdr->e_ident[2] = 'L';
    ehdr->e_ident[3] = 'F';
    ehdr->e_ident[4] = 2;
    ehdr->e_ident[5] = 1;
    ehdr->e_type = 2;
    ehdr->e_machine = 62;
    ehdr->e_version = 1;
    ehdr->e_entry = entry;
    ehdr->e_phoff = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 1;
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);

    Elf64_Phdr *phdr = (Elf64_Phdr *)(buf + sizeof(Elf64_Ehdr));
    phdr->p_type = 1;
    phdr->p_flags = p_flags;
    phdr->p_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    phdr->p_vaddr = vaddr;
    phdr->p_paddr = vaddr;
    phdr->p_filesz = filesz;
    phdr->p_memsz = memsz;
    phdr->p_align = 4096;

    if (filesz > 0) {
        uint8_t *data_buf = buf + phdr->p_offset;
        for (uint64_t i = 0; i < filesz && (phdr->p_offset + i) < buf_size; i++) {
            data_buf[i] = (uint8_t)(i & 0xFF);
        }
    }
}

/* --- Tests --- */

TEST(bad_magic) {
    reset_stubs();
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);
    return 0;
}

TEST(wrong_class) {
    reset_stubs();
    uint8_t buf[512];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, 5, 0, 4096);
    buf[4] = 1;
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);
    return 0;
}

TEST(wrong_machine) {
    reset_stubs();
    uint8_t buf[512];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, 5, 0, 4096);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    ehdr->e_machine = 3;
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);
    return 0;
}

TEST(not_executable) {
    reset_stubs();
    uint8_t buf[512];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, 5, 0, 4096);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    ehdr->e_type = 1;
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);
    return 0;
}

TEST(null_data) {
    ElfLoadResult result;
    ASSERT_EQ(elf_load(NULL, 100, 0x200000, &result), -ENOEXEC);
    return 0;
}

TEST(too_small) {
    reset_stubs();
    uint8_t buf[8];
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);
    return 0;
}

TEST(valid_single_segment) {
    reset_stubs();
    uint8_t buf[4096];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400100, 5, 256, 4096);
    ElfLoadResult result;
    int err = elf_load(buf, sizeof(buf), 0x200000, &result);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(result.entry_point, 0x400100);
    ASSERT_TRUE(result.brk_start >= 0x400000 + 4096);
    ASSERT_EQ(result.brk_start & 0xFFF, 0);
    ASSERT_EQ(map_call_count, 1);
    ASSERT_EQ(map_calls[0].pml4, 0x200000);
    ASSERT_EQ(map_calls[0].virt, 0x400000);
    return 0;
}

TEST(segment_maps_correct_flags) {
    reset_stubs();
    uint8_t buf[4096];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, 6, 100, 4096);
    ElfLoadResult result;
    int err = elf_load(buf, sizeof(buf), 0x200000, &result);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(map_call_count, 1);
    uint32_t expected = VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC;
    ASSERT_EQ(map_calls[0].flags, expected);
    return 0;
}

TEST(kernel_space_rejected) {
    reset_stubs();
    uint8_t buf[4096];
    build_minimal_elf(buf, sizeof(buf), 0xFFFF800000000000ULL, 0xFFFF800000000000ULL, 5, 0, 4096);
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);
    return 0;
}

TEST(multi_page_segment) {
    reset_stubs();
    uint8_t buf[8192];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, 5, 100, 8192);
    ElfLoadResult result;
    int err = elf_load(buf, sizeof(buf), 0x200000, &result);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(map_call_count, 2);
    ASSERT_EQ(map_calls[0].virt, 0x400000);
    ASSERT_EQ(map_calls[1].virt, 0x401000);
    return 0;
}

/* Append a program header to a buffer built by build_minimal_elf */
static Elf64_Phdr *add_phdr(uint8_t *buf, uint32_t type, uint32_t flags) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    Elf64_Phdr *ph = (Elf64_Phdr *)(buf + ehdr->e_phoff) + ehdr->e_phnum;
    memset(ph, 0, sizeof(*ph));
    ph->p_type = type;
    ph->p_flags = flags;
    ehdr->e_phnum++;
    return ph;
}

/* Two segments: build_minimal_elf puts the data right after one phdr, so
 * leave room for four headers and move the data there */
static void build_two_segment_elf(uint8_t *buf, size_t size) {
    build_minimal_elf(buf, size, 0x400000, 0x400000, PF_R | PF_X, 0, 0);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    ehdr->e_phnum = 0;
    /* text: file 0x100..0x900 at 0x400100, r-x */
    Elf64_Phdr *text = add_phdr(buf, PT_LOAD, PF_R | PF_X);
    text->p_offset = 0x100; text->p_vaddr = 0x400100;
    text->p_filesz = 0x800; text->p_memsz = 0x800;
    /* data: file 0x900..0xA00 at 0x400900 (same page), rw-, bss to 0x401100 */
    Elf64_Phdr *data = add_phdr(buf, PT_LOAD, PF_R | PF_W);
    data->p_offset = 0x900; data->p_vaddr = 0x400900;
    data->p_filesz = 0x100; data->p_memsz = 0x800;
    memset(buf + 0x100, 0xAA, 0x800);
    memset(buf + 0x900, 0xBB, 0x100);
}

TEST(shared_page_keeps_both_segments) {
    reset_stubs();
    static uint8_t buf[4096];
    build_two_segment_elf(buf, sizeof(buf));
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), 0);
    ASSERT_EQ(allocated_page_count, 2);     /* 0x400000 once, 0x401000 */
    ASSERT_EQ(mapping_count, 2);
    int i = mapping_find(0x400000);
    ASSERT_TRUE(i >= 0);
    uint8_t *page = (uint8_t *)(uintptr_t)mappings[i].phys;
    ASSERT_EQ(page[0x100], 0xAA);            /* text survived the data segment */
    ASSERT_EQ(page[0x8FF], 0xAA);
    ASSERT_EQ(page[0x900], 0xBB);
    ASSERT_EQ(page[0xA00], 0);               /* bss */
    /* The shared page allows both: writable and executable */
    ASSERT_EQ(mappings[i].flags, VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    i = mapping_find(0x401000);
    ASSERT_EQ(mappings[i].flags, VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
    return 0;
}

TEST(interp_rejected) {
    reset_stubs();
    static uint8_t buf[4096];
    build_two_segment_elf(buf, sizeof(buf));
    add_phdr(buf, PT_INTERP, PF_R);
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);
    ASSERT_EQ(map_call_count, 0);            /* refused before mapping anything */
    return 0;
}

TEST(static_pie_loads_at_base) {
    reset_stubs();
    static uint8_t buf[4096];
    build_minimal_elf(buf, sizeof(buf), 0, 0x120, PF_R | PF_X, 0x200, 0x200);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    ehdr->e_type = ET_DYN;
    Elf64_Phdr *ph = (Elf64_Phdr *)(buf + ehdr->e_phoff);
    ph->p_offset = 0;                        /* covers the headers too */
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), 0);
    ASSERT_EQ(result.load_bias, ELF_PIE_BASE);
    ASSERT_EQ(result.entry_point, ELF_PIE_BASE + 0x120);
    ASSERT_EQ(result.phdr_addr, ELF_PIE_BASE + sizeof(Elf64_Ehdr));
    ASSERT_EQ(map_calls[0].virt, ELF_PIE_BASE);
    ASSERT_EQ(result.brk_start, ELF_PIE_BASE + 0x1000);
    return 0;
}

TEST(phdr_from_pt_phdr_or_first_segment) {
    reset_stubs();
    static uint8_t buf[4096];
    /* Headers not inside any segment: AT_PHDR unknown (0) */
    build_two_segment_elf(buf, sizeof(buf));
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), 0);
    ASSERT_EQ(result.phdr_addr, 0);
    ASSERT_EQ(result.phnum, 2);
    ASSERT_EQ(result.phent, sizeof(Elf64_Phdr));

    /* PT_PHDR names them */
    reset_stubs();
    build_two_segment_elf(buf, sizeof(buf));
    Elf64_Phdr *ph = add_phdr(buf, PT_PHDR, PF_R);
    ph->p_vaddr = 0x400040;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), 0);
    ASSERT_EQ(result.phdr_addr, 0x400040);
    return 0;
}

TEST(gnu_stack_flag) {
    reset_stubs();
    static uint8_t buf[4096];
    build_two_segment_elf(buf, sizeof(buf));
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), 0);
    ASSERT_EQ(result.stack_exec, 1);         /* no PT_GNU_STACK: Linux default */
    reset_stubs();
    build_two_segment_elf(buf, sizeof(buf));
    add_phdr(buf, PT_GNU_STACK, PF_R | PF_W);
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), 0);
    ASSERT_EQ(result.stack_exec, 0);
    return 0;
}

TEST(malformed_headers_rejected) {
    static uint8_t buf[4096];
    ElfLoadResult result;

    reset_stubs();
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, PF_R, 0x100, 0x80);
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);  /* filesz > memsz */

    reset_stubs();
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, PF_R, 0, 0x1000);
    ((Elf64_Ehdr *)buf)->e_phentsize = 32;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);

    reset_stubs();
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, PF_R, 0, 0x1000);
    ((Elf64_Ehdr *)buf)->e_phoff = UINT64_MAX - 8;   /* would wrap */
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);

    reset_stubs();
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, PF_R, 0x100, 0x1000);
    ((Elf64_Phdr *)(buf + sizeof(Elf64_Ehdr)))->p_offset = UINT64_MAX - 8;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);

    reset_stubs();
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, PF_R, 0, 0);  /* no loadable */
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);

    reset_stubs();
    build_minimal_elf(buf, sizeof(buf), 0x7FFFFFFFF000ULL, 0x400000, PF_R, 0, 0x2000);
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -ENOEXEC);  /* crosses the top */
    return 0;
}

/* --- Suite --- */

TestCase elf_tests[] = {
    TEST_ENTRY(bad_magic),
    TEST_ENTRY(wrong_class),
    TEST_ENTRY(wrong_machine),
    TEST_ENTRY(not_executable),
    TEST_ENTRY(null_data),
    TEST_ENTRY(too_small),
    TEST_ENTRY(valid_single_segment),
    TEST_ENTRY(segment_maps_correct_flags),
    TEST_ENTRY(kernel_space_rejected),
    TEST_ENTRY(multi_page_segment),
    TEST_ENTRY(shared_page_keeps_both_segments),
    TEST_ENTRY(interp_rejected),
    TEST_ENTRY(static_pie_loads_at_base),
    TEST_ENTRY(phdr_from_pt_phdr_or_first_segment),
    TEST_ENTRY(gnu_stack_flag),
    TEST_ENTRY(malformed_headers_rejected),
};
int elf_test_count = sizeof(elf_tests) / sizeof(elf_tests[0]);
