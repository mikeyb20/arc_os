/* arc_os — Host-side tests for ELF64 loader */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Guard kernel headers — we provide stubs */
#define ARCHOS_MM_VMM_H
#define ARCHOS_MM_PMM_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_FS_VFS_H
#define ARCHOS_PROC_ELF_H

/* Constants elf.c needs */
#define PAGE_SIZE 4096
#define PAGE_ALIGN_UP(x)    (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL))
#define PAGE_ALIGN_DOWN(x)  ((x) & ~(PAGE_SIZE - 1ULL))
#define VMM_FLAG_WRITABLE  (1 << 0)
#define VMM_FLAG_USER      (1 << 1)
#define VMM_FLAG_NOEXEC    (1 << 2)
#define EINVAL 22
#define ENOMEM 12

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Track vmm_map_page_in calls */
#define MAX_MAP_CALLS 64
static struct {
    uint64_t pml4;
    uint64_t virt;
    uint64_t phys;
    uint32_t flags;
} map_calls[MAX_MAP_CALLS];
static int map_call_count = 0;

/* Stub PMM: allocate real heap memory and return its address as "physical" */
#define MAX_PAGES 32
static void *allocated_pages[MAX_PAGES];
static int allocated_page_count = 0;

static uint64_t pmm_alloc_page(void) {
    if (allocated_page_count >= MAX_PAGES) return 0;
    void *p = calloc(1, PAGE_SIZE);
    if (!p) return 0;
    allocated_pages[allocated_page_count++] = p;
    return (uint64_t)(uintptr_t)p;
}

/* Stub VMM: hhdm offset is 0 so phys == virt (the "phys" is already a host pointer) */
static uint64_t vmm_get_hhdm_offset(void) { return 0; }

static void vmm_map_page_in(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t flags) {
    if (map_call_count < MAX_MAP_CALLS) {
        map_calls[map_call_count].pml4 = pml4;
        map_calls[map_call_count].virt = virt;
        map_calls[map_call_count].phys = phys;
        map_calls[map_call_count].flags = flags;
        map_call_count++;
    }
}

/* Reset test state */
static void reset_stubs(void) {
    map_call_count = 0;
    memset(map_calls, 0, sizeof(map_calls));
    /* Free allocated pages */
    for (int i = 0; i < allocated_page_count; i++) {
        free(allocated_pages[i]);
        allocated_pages[i] = NULL;
    }
    allocated_page_count = 0;
}

/* ELF64 types (reproduced since we guard elf.h) */
#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    uint64_t entry_point;
    uint64_t brk_start;
} ElfLoadResult;

/* ELF constants */
#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_NULL     0
#define PT_LOAD     1
#define PF_X        (1 << 0)
#define PF_W        (1 << 1)
#define PF_R        (1 << 2)

/* Declare elf_load before including the implementation */
int elf_load(const void *data, size_t size, uint64_t pml4_phys, ElfLoadResult *result);

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
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -EINVAL);
    return 0;
}

TEST(wrong_class) {
    reset_stubs();
    uint8_t buf[512];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, 5, 0, 4096);
    buf[4] = 1;
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -EINVAL);
    return 0;
}

TEST(wrong_machine) {
    reset_stubs();
    uint8_t buf[512];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, 5, 0, 4096);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    ehdr->e_machine = 3;
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -EINVAL);
    return 0;
}

TEST(not_executable) {
    reset_stubs();
    uint8_t buf[512];
    build_minimal_elf(buf, sizeof(buf), 0x400000, 0x400000, 5, 0, 4096);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    ehdr->e_type = 1;
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -EINVAL);
    return 0;
}

TEST(null_data) {
    ElfLoadResult result;
    ASSERT_EQ(elf_load(NULL, 100, 0x200000, &result), -EINVAL);
    return 0;
}

TEST(too_small) {
    reset_stubs();
    uint8_t buf[8];
    ElfLoadResult result;
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -EINVAL);
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
    ASSERT_EQ(elf_load(buf, sizeof(buf), 0x200000, &result), -EINVAL);
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
};
int elf_test_count = sizeof(elf_tests) / sizeof(elf_tests[0]);
