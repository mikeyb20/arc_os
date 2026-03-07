#include "proc/elf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "fs/vfs.h"  /* For error codes */

/* Maximum user-space virtual address (canonical lower half) */
#define USER_VADDR_MAX 0x0000800000000000ULL

int elf_load(const void *data, size_t size, uint64_t pml4_phys, ElfLoadResult *result) {
    if (data == NULL || result == NULL || size < sizeof(Elf64_Ehdr)) {
        return -EINVAL;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    /* Validate ELF magic */
    if (ehdr->e_ident[EI_MAG0] != ELF_MAGIC0 ||
        ehdr->e_ident[EI_MAG1] != ELF_MAGIC1 ||
        ehdr->e_ident[EI_MAG2] != ELF_MAGIC2 ||
        ehdr->e_ident[EI_MAG3] != ELF_MAGIC3) {
        kprintf("[ELF] Bad magic\n");
        return -EINVAL;
    }

    /* Validate ELF class and encoding */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        kprintf("[ELF] Not 64-bit\n");
        return -EINVAL;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf("[ELF] Not little-endian\n");
        return -EINVAL;
    }

    /* Validate type and machine */
    if (ehdr->e_type != ET_EXEC) {
        kprintf("[ELF] Not an executable (type=%u)\n", ehdr->e_type);
        return -EINVAL;
    }
    if (ehdr->e_machine != EM_X86_64) {
        kprintf("[ELF] Not x86_64 (machine=%u)\n", ehdr->e_machine);
        return -EINVAL;
    }

    /* Validate program header table bounds */
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        kprintf("[ELF] No program headers\n");
        return -EINVAL;
    }
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size) {
        kprintf("[ELF] Program headers extend past file\n");
        return -EINVAL;
    }

    uint64_t highest_addr = 0;
    uint64_t hhdm = vmm_get_hhdm_offset();

    /* Process each program header */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)
            ((const uint8_t *)data + ehdr->e_phoff + i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;

        /* Verify segment is in user space */
        if (phdr->p_vaddr >= USER_VADDR_MAX ||
            phdr->p_vaddr + phdr->p_memsz > USER_VADDR_MAX) {
            kprintf("[ELF] Segment vaddr 0x%lx outside user space\n", phdr->p_vaddr);
            return -EINVAL;
        }

        /* Verify file data bounds */
        if (phdr->p_filesz > 0 &&
            phdr->p_offset + phdr->p_filesz > size) {
            kprintf("[ELF] Segment file data extends past file\n");
            return -EINVAL;
        }

        /* Convert ELF flags to VMM flags */
        uint32_t vmm_flags = VMM_FLAG_USER;
        if (phdr->p_flags & PF_W) vmm_flags |= VMM_FLAG_WRITABLE;
        if (!(phdr->p_flags & PF_X)) vmm_flags |= VMM_FLAG_NOEXEC;

        /* Map pages for this segment */
        uint64_t seg_start = phdr->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end = (phdr->p_vaddr + phdr->p_memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

        for (uint64_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (phys == 0) {
                kprintf("[ELF] Out of memory mapping segment\n");
                return -ENOMEM;
            }

            /* Zero the page first (handles BSS and partial pages) */
            void *page_virt = (void *)(phys + hhdm);
            memset(page_virt, 0, PAGE_SIZE);

            /* Copy file data if this page overlaps with the file portion */
            if (phdr->p_filesz > 0) {
                uint64_t file_start = phdr->p_vaddr;
                uint64_t file_end = phdr->p_vaddr + phdr->p_filesz;

                /* Calculate overlap between this page and file data */
                uint64_t page_start = vaddr;
                uint64_t page_end = vaddr + PAGE_SIZE;

                uint64_t copy_start = (file_start > page_start) ? file_start : page_start;
                uint64_t copy_end = (file_end < page_end) ? file_end : page_end;

                if (copy_start < copy_end) {
                    uint64_t file_offset = phdr->p_offset + (copy_start - phdr->p_vaddr);
                    uint64_t page_offset = copy_start - page_start;
                    uint64_t copy_len = copy_end - copy_start;

                    memcpy((uint8_t *)page_virt + page_offset,
                           (const uint8_t *)data + file_offset,
                           copy_len);
                }
            }

            vmm_map_page_in(pml4_phys, vaddr, phys, vmm_flags);
        }

        /* Track highest loaded address for brk */
        uint64_t seg_top = phdr->p_vaddr + phdr->p_memsz;
        if (seg_top > highest_addr) {
            highest_addr = seg_top;
        }

        kprintf("[ELF] Loaded segment: vaddr=0x%lx memsz=0x%lx filesz=0x%lx flags=%s%s%s\n",
                phdr->p_vaddr, phdr->p_memsz, phdr->p_filesz,
                (phdr->p_flags & PF_R) ? "r" : "-",
                (phdr->p_flags & PF_W) ? "w" : "-",
                (phdr->p_flags & PF_X) ? "x" : "-");
    }

    result->entry_point = ehdr->e_entry;
    result->brk_start = (highest_addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    kprintf("[ELF] Loaded: entry=0x%lx brk_start=0x%lx\n",
            result->entry_point, result->brk_start);
    return 0;
}
