#include "proc/elf.h"
#include "mm/mm.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "fs/vfs.h"  /* For error codes */

/* Maximum user-space virtual address (canonical lower half) */
#define USER_VADDR_MAX 0x0000800000000000ULL

static int elf_validate_header(const Elf64_Ehdr *ehdr, uint64_t size) {
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
    if ((uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > ELF_MAX_PHDR_BYTES) {
        kprintf("[ELF] Too many program headers\n");
        return -ENOEXEC;
    }
    if (ehdr->e_phoff > size ||
        (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > size - ehdr->e_phoff) {
        kprintf("[ELF] Program headers extend past file\n");
        return -ENOEXEC;
    }
    return 0;
}

/* --- Sources --- */

static int mem_read(const ElfSource *src, uint64_t off, void *dst, size_t len) {
    memcpy(dst, (const uint8_t *)src->data + off, len);
    return 0;
}

void elf_source_mem(ElfSource *src, const void *data, uint64_t size) {
    memset(src, 0, sizeof(*src));
    src->size = size;
    src->data = data;
    src->read = mem_read;
}

/* Fill the bytes of page_virt (mapped at vaddr) that the segment's file data
 * covers. 0 or the source's error. */
static int elf_fill_page(const ElfSource *src, void *page_virt, uint64_t vaddr, const Elf64_Phdr *phdr, uint64_t bias) {
    if (phdr->p_filesz == 0) return 0;
    uint64_t file_start = phdr->p_vaddr + bias;
    uint64_t file_end = file_start + phdr->p_filesz;
    uint64_t copy_start = file_start > vaddr ? file_start : vaddr;
    uint64_t copy_end = file_end < vaddr + PAGE_SIZE ? file_end : vaddr + PAGE_SIZE;
    if (copy_start >= copy_end) return 0;
    return src->read(src, phdr->p_offset + (copy_start - file_start), (uint8_t *)page_virt + (copy_start - vaddr),
                     (size_t)(copy_end - copy_start));
}

/* Load a single PT_LOAD segment: validate bounds, add its VMAs, fill the
 * pages that must be copied.
 *
 * From a source with mapping ops (a file in the page cache, M1-12) the whole
 * pages of file data become a private file VMA, read on first touch and
 * shared with every other process running the program (a write copies the
 * page). Copied now into anonymous pages: a first page that the previous
 * segment also covers (the end of .text and the start of .data), and the
 * last, partly-file page (its tail is .bss, zero). From a memory source every
 * page with file data is copied. Pages past the file data (.bss) are
 * anonymous and read as zero on first touch. */
static int elf_load_segment(const ElfSource *src, const Elf64_Phdr *phdr, uint64_t bias, Mm *mm, uint64_t hhdm,
                            uint64_t prev_end, uint64_t *highest_addr) {
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
    if (phdr->p_filesz > 0 && (phdr->p_offset > src->size || phdr->p_filesz > src->size - phdr->p_offset)) {
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

    /* [map_start, map_end): whole pages of file data mapped from the file */
    uint64_t map_start = prev_end > seg_start ? seg_start + PAGE_SIZE : seg_start;
    uint64_t map_end = PAGE_ALIGN_DOWN(start + phdr->p_filesz);
    if (src->ops == NULL || (phdr->p_offset - start) % PAGE_SIZE != 0 || map_end <= map_start)
        map_end = map_start;   /* nothing mapped: copy (a file whose offsets and addresses disagree too) */

    int err = 0;
    if (map_end > map_start) {
        if (map_start > seg_start) err = mm_setup_vma(mm, seg_start, map_start, prot, 0);
        if (err == 0)
            err = mm_setup_object_vma(mm, map_start, map_end, prot, src->file,
                                      (phdr->p_offset + (map_start - start)) / PAGE_SIZE, src->ops);
        if (err == 0 && seg_end > map_end) err = mm_setup_vma(mm, map_end, seg_end, prot, 0);
    } else {
        err = mm_setup_vma(mm, seg_start, seg_end, prot, 0);
    }
    if (err != 0) {
        kprintf("[ELF] Segment at 0x%lx overlaps another\n", start);
        return err == -ENOMEM ? -ENOMEM : -ENOEXEC;
    }

    /* Pages with file bytes outside the mapped range now; the rest on first touch */
    uint64_t data_end = PAGE_ALIGN_UP(start + phdr->p_filesz);
    for (uint64_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
        if (vaddr == map_start && map_end > map_start) {
            vaddr = map_end - PAGE_SIZE;
            continue;
        }
        uint64_t phys;
        uint32_t flags = vmm_flags;
        int old = vmm_get_flags_in(mm->pgd, vaddr);
        if (old >= 0) {
            /* A page the previous segment also covers (e.g. the end of .text
             * and the start of .data): keep its contents, allow both. */
            phys = PAGE_ALIGN_DOWN(vmm_get_phys_in(mm->pgd, vaddr));
            flags |= (uint32_t)old & VMM_FLAG_WRITABLE;
            if (!((uint32_t)old & VMM_FLAG_NOEXEC)) flags &= ~(uint32_t)VMM_FLAG_NOEXEC;
            err = elf_fill_page(src, (void *)(phys + hhdm), vaddr, phdr, bias);
            if (err != 0) return err;
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
        err = elf_fill_page(src, (void *)(phys + hhdm), vaddr, phdr, bias);
        if (err == 0 && mm_setup_page(mm, vaddr, phys, flags) != 0) err = -ENOMEM;
        if (err != 0) {
            pmm_free_page(phys);
            return err;
        }
    }

    /* Track highest loaded address for brk */
    uint64_t seg_top = start + phdr->p_memsz;
    if (seg_top > *highest_addr) {
        *highest_addr = seg_top;
    }

    kdebug("[ELF] Loaded segment: vaddr=0x%lx memsz=0x%lx filesz=0x%lx flags=%s%s%s mapped=%lu pages\n", start,
           phdr->p_memsz, phdr->p_filesz, (phdr->p_flags & PF_R) ? "r" : "-", (phdr->p_flags & PF_W) ? "w" : "-",
           (phdr->p_flags & PF_X) ? "x" : "-", (map_end - map_start) / PAGE_SIZE);
    return 0;
}

/* Pass 1: refuse dynamic programs, find the lowest vaddr, PT_PHDR and
 * PT_GNU_STACK. Linux defaults to an executable stack without PT_GNU_STACK. */
static int elf_scan(const Elf64_Phdr *ph, uint16_t phnum, uint64_t *lowest, uint64_t *phdr_vaddr, int *have_phdr,
                    int *stack_exec) {
    *lowest = UINT64_MAX;
    *have_phdr = 0;
    *stack_exec = 1;
    for (uint16_t i = 0; i < phnum; i++) {
        switch (ph[i].p_type) {
        case PT_INTERP:
            kprintf("[ELF] Dynamically linked (PT_INTERP): only static programs run (ADR-0002)\n");
            return -ENOEXEC;
        case PT_LOAD:
            if (ph[i].p_memsz != 0 && ph[i].p_vaddr < *lowest) *lowest = ph[i].p_vaddr;
            break;
        case PT_PHDR:
            *phdr_vaddr = ph[i].p_vaddr;
            *have_phdr = 1;
            break;
        case PT_GNU_STACK:
            *stack_exec = (ph[i].p_flags & PF_X) != 0;
            break;
        default:
            break;
        }
    }
    if (*lowest == UINT64_MAX) {
        kprintf("[ELF] No loadable segments\n");
        return -ENOEXEC;
    }
    return 0;
}

static int elf_load_phdrs(const ElfSource *src, const Elf64_Ehdr *ehdr, const Elf64_Phdr *ph, Mm *mm,
                          ElfLoadResult *result) {
    uint64_t lowest, phdr_vaddr = 0;
    int have_phdr, stack_exec;
    int err = elf_scan(ph, ehdr->e_phnum, &lowest, &phdr_vaddr, &have_phdr, &stack_exec);
    if (err != 0) return err;

    uint64_t bias = 0;
    if (ehdr->e_type == ET_DYN) {
        if (PAGE_ALIGN_DOWN(lowest) > ELF_PIE_BASE) return -ENOEXEC;
        bias = ELF_PIE_BASE - PAGE_ALIGN_DOWN(lowest);
    }

    uint64_t highest_addr = 0;
    uint64_t hhdm = vmm_get_hhdm_offset();
    uint64_t prev_end = 0;   /* end (page aligned) of the segment loaded last */

    /* Pass 2: map every PT_LOAD segment */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = &ph[i];
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) continue;
        err = elf_load_segment(src, phdr, bias, mm, hhdm, prev_end, &highest_addr);
        if (err != 0) return err;
        prev_end = PAGE_ALIGN_UP(phdr->p_vaddr + bias + phdr->p_memsz);

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

int elf_load_from(const ElfSource *src, Mm *mm, ElfLoadResult *result) {
    if (src == NULL || result == NULL || src->size < sizeof(Elf64_Ehdr)) return -ENOEXEC;
    Elf64_Ehdr ehdr;
    int err = src->read(src, 0, &ehdr, sizeof(ehdr));
    if (err != 0) return err;
    err = elf_validate_header(&ehdr, src->size);
    if (err != 0) return err;
    size_t ph_bytes = (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr);   /* <= ELF_MAX_PHDR_BYTES */
    Elf64_Phdr *ph = kmalloc(ph_bytes, 0);
    if (ph == NULL) return -ENOMEM;
    err = src->read(src, ehdr.e_phoff, ph, ph_bytes);
    if (err == 0) err = elf_load_phdrs(src, &ehdr, ph, mm, result);
    kfree(ph);
    return err;
}

int elf_load(const void *data, size_t size, Mm *mm, ElfLoadResult *result) {
    if (data == NULL) return -ENOEXEC;
    ElfSource src;
    elf_source_mem(&src, data, size);
    return elf_load_from(&src, mm, result);
}
