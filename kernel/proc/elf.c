#include "proc/elf.h"
#include "mm/mm.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "fs/vfs.h"  /* For error codes */

/* Maximum user-space virtual address (canonical lower half) */
#define USER_VADDR_MAX 0x0000800000000000ULL

static int elf_validate_header(const Elf64_Ehdr *ehdr, size_t size) {
    if (ehdr->e_ident[EI_MAG0] != ELF_MAGIC0 ||
        ehdr->e_ident[EI_MAG1] != ELF_MAGIC1 ||
        ehdr->e_ident[EI_MAG2] != ELF_MAGIC2 ||
        ehdr->e_ident[EI_MAG3] != ELF_MAGIC3) {
        kprintf("[ELF] Bad magic\n");
        return -ENOEXEC;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        kprintf("[ELF] Not 64-bit\n");
        return -ENOEXEC;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf("[ELF] Not little-endian\n");
        return -ENOEXEC;
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        kprintf("[ELF] Not an executable (type=%u)\n", ehdr->e_type);
        return -ENOEXEC;
    }
    if (ehdr->e_machine != EM_X86_64) {
        kprintf("[ELF] Not x86_64 (machine=%u)\n", ehdr->e_machine);
        return -ENOEXEC;
    }
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0 ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        kprintf("[ELF] No usable program headers\n");
        return -ENOEXEC;
    }
    if (ehdr->e_phoff > size ||
        (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > size - ehdr->e_phoff) {
        kprintf("[ELF] Program headers extend past file\n");
        return -ENOEXEC;
    }
    return 0;
}

static const Elf64_Phdr *elf_phdr(const void *data, const Elf64_Ehdr *ehdr, uint16_t i) {
    return (const Elf64_Phdr *)((const uint8_t *)data + ehdr->e_phoff +
                                (uint64_t)i * sizeof(Elf64_Phdr));
}

/* Copy file data that overlaps with a single mapped page. */
static void elf_copy_page_data(void *page_virt, uint64_t vaddr,
                               const Elf64_Phdr *phdr, uint64_t bias, const void *data) {
    if (phdr->p_filesz == 0) return;

    uint64_t file_start = phdr->p_vaddr + bias;
    uint64_t file_end   = file_start + phdr->p_filesz;
    uint64_t page_start = vaddr;
    uint64_t page_end   = vaddr + PAGE_SIZE;

    uint64_t copy_start = (file_start > page_start) ? file_start : page_start;
    uint64_t copy_end   = (file_end < page_end) ? file_end : page_end;

    if (copy_start < copy_end) {
        uint64_t file_offset = phdr->p_offset + (copy_start - file_start);
        uint64_t page_offset = copy_start - page_start;
        memcpy((uint8_t *)page_virt + page_offset,
               (const uint8_t *)data + file_offset,
               copy_end - copy_start);
    }
}

/* Load a single PT_LOAD segment: validate bounds, add its VMA, map and fill
 * the pages that hold file data. Pages past the file data (.bss) are left to
 * demand paging: the VMA is anonymous, so they read as zero (M1-07). */
static int elf_load_segment(const Elf64_Phdr *phdr, uint64_t bias, const void *data, size_t file_size, Mm *mm,
                            uint64_t hhdm, uint64_t *highest_addr) {
    uint64_t start = phdr->p_vaddr + bias;

    /* Verify segment is in user space (and the sums do not wrap) */
    if (start < phdr->p_vaddr || start >= USER_VADDR_MAX ||
        phdr->p_memsz > USER_VADDR_MAX - start) {
        kprintf("[ELF] Segment vaddr 0x%lx outside user space\n", start);
        return -ENOEXEC;
    }
    if (phdr->p_filesz > phdr->p_memsz) {
        kprintf("[ELF] Segment filesz > memsz\n");
        return -ENOEXEC;
    }

    /* Verify file data bounds */
    if (phdr->p_filesz > 0 &&
        (phdr->p_offset > file_size || phdr->p_filesz > file_size - phdr->p_offset)) {
        kprintf("[ELF] Segment file data extends past file\n");
        return -ENOEXEC;
    }

    /* Convert ELF flags to VMM flags */
    uint32_t vmm_flags = VMM_FLAG_USER;
    if (phdr->p_flags & PF_W) vmm_flags |= VMM_FLAG_WRITABLE;
    if (!(phdr->p_flags & PF_X)) vmm_flags |= VMM_FLAG_NOEXEC;

    uint64_t seg_start = PAGE_ALIGN_DOWN(start);
    uint64_t seg_end = PAGE_ALIGN_UP(start + phdr->p_memsz);
    uint32_t prot = ((phdr->p_flags & PF_R) ? VMA_READ : 0) | ((phdr->p_flags & PF_W) ? VMA_WRITE : 0) |
                    ((phdr->p_flags & PF_X) ? VMA_EXEC : 0);
    int err = mm_setup_vma(mm, seg_start, seg_end, prot, 0);
    if (err != 0) {
        kprintf("[ELF] Segment at 0x%lx overlaps another\n", start);
        return err == -ENOMEM ? -ENOMEM : -ENOEXEC;
    }

    /* Pages with file bytes now; the rest on first touch */
    uint64_t data_end = PAGE_ALIGN_UP(start + phdr->p_filesz);
    for (uint64_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
        uint64_t phys;
        uint32_t flags = vmm_flags;
        int old = vmm_get_flags_in(mm->pgd, vaddr);
        if (old >= 0) {
            /* A page the previous segment also covers (e.g. the end of .text
             * and the start of .data): keep its contents, allow both. */
            phys = PAGE_ALIGN_DOWN(vmm_get_phys_in(mm->pgd, vaddr));
            flags |= (uint32_t)old & VMM_FLAG_WRITABLE;
            if (!((uint32_t)old & VMM_FLAG_NOEXEC)) flags &= ~(uint32_t)VMM_FLAG_NOEXEC;
            elf_copy_page_data((void *)(phys + hhdm), vaddr, phdr, bias, data);
            if (vmm_map_page_in(mm->pgd, vaddr, phys, flags) != 0) return -ENOMEM;
            continue;
        }
        if (phdr->p_filesz == 0 || vaddr >= data_end) continue;   /* .bss only */
        phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[ELF] Out of memory mapping segment\n");
            return -ENOMEM;
        }
        memset((void *)(phys + hhdm), 0, PAGE_SIZE);
        elf_copy_page_data((void *)(phys + hhdm), vaddr, phdr, bias, data);
        if (mm_setup_page(mm, vaddr, phys, flags) != 0) {
            pmm_free_page(phys);
            return -ENOMEM;
        }
    }

    /* Track highest loaded address for brk */
    uint64_t seg_top = start + phdr->p_memsz;
    if (seg_top > *highest_addr) {
        *highest_addr = seg_top;
    }

    kdebug("[ELF] Loaded segment: vaddr=0x%lx memsz=0x%lx filesz=0x%lx flags=%s%s%s\n", start, phdr->p_memsz,
           phdr->p_filesz, (phdr->p_flags & PF_R) ? "r" : "-", (phdr->p_flags & PF_W) ? "w" : "-",
           (phdr->p_flags & PF_X) ? "x" : "-");
    return 0;
}

int elf_load(const void *data, size_t size, Mm *mm, ElfLoadResult *result) {
    if (data == NULL || result == NULL || size < sizeof(Elf64_Ehdr)) {
        return -ENOEXEC;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    int err = elf_validate_header(ehdr, size);
    if (err != 0) return err;

    /* Pass 1: refuse dynamic programs, find the lowest vaddr, PT_PHDR and
     * PT_GNU_STACK. Linux defaults to an executable stack without PT_GNU_STACK. */
    uint64_t lowest = UINT64_MAX;
    uint64_t phdr_vaddr = 0;
    int have_phdr = 0;
    int stack_exec = 1;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_phdr(data, ehdr, i);
        switch (phdr->p_type) {
        case PT_INTERP:
            kprintf("[ELF] Dynamically linked (PT_INTERP): only static programs run (ADR-0002)\n");
            return -ENOEXEC;
        case PT_LOAD:
            if (phdr->p_memsz != 0 && phdr->p_vaddr < lowest) lowest = phdr->p_vaddr;
            break;
        case PT_PHDR:
            phdr_vaddr = phdr->p_vaddr;
            have_phdr = 1;
            break;
        case PT_GNU_STACK:
            stack_exec = (phdr->p_flags & PF_X) != 0;
            break;
        default:
            break;
        }
    }
    if (lowest == UINT64_MAX) {
        kprintf("[ELF] No loadable segments\n");
        return -ENOEXEC;
    }

    uint64_t bias = 0;
    if (ehdr->e_type == ET_DYN) {
        if (PAGE_ALIGN_DOWN(lowest) > ELF_PIE_BASE) return -ENOEXEC;
        bias = ELF_PIE_BASE - PAGE_ALIGN_DOWN(lowest);
    }

    uint64_t highest_addr = 0;
    uint64_t hhdm = vmm_get_hhdm_offset();

    /* Pass 2: map every PT_LOAD segment */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_phdr(data, ehdr, i);
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) continue;
        err = elf_load_segment(phdr, bias, data, size, mm, hhdm, &highest_addr);
        if (err != 0) return err;

        /* No PT_PHDR: the headers are wherever the segment holding e_phoff put them */
        uint64_t ph_bytes = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
        if (!have_phdr && phdr->p_offset <= ehdr->e_phoff &&
            ehdr->e_phoff - phdr->p_offset + ph_bytes <= phdr->p_filesz) {
            phdr_vaddr = phdr->p_vaddr + (ehdr->e_phoff - phdr->p_offset);
            have_phdr = 1;
        }
    }

    uint64_t entry = ehdr->e_entry + bias;
    if (entry >= USER_VADDR_MAX || entry < ehdr->e_entry) {
        kprintf("[ELF] Entry point 0x%lx outside user space\n", entry);
        return -ENOEXEC;
    }

    result->entry_point = entry;
    result->brk_start = PAGE_ALIGN_UP(highest_addr);
    result->load_bias = bias;
    result->phdr_addr = have_phdr ? phdr_vaddr + bias : 0;
    result->phnum = ehdr->e_phnum;
    result->phent = (uint16_t)sizeof(Elf64_Phdr);
    result->stack_exec = (uint8_t)stack_exec;

    kdebug("[ELF] Loaded: %s entry=0x%lx brk_start=0x%lx\n", ehdr->e_type == ET_DYN ? "static-PIE" : "static", entry,
           result->brk_start);
    return 0;
}
