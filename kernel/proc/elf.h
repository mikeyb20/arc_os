#ifndef ARCHOS_PROC_ELF_H
#define ARCHOS_PROC_ELF_H

#include <stdint.h>
#include <stddef.h>

/* ELF64 magic bytes */
#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

/* ELF identification indices */
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_NIDENT   16

/* ELF class */
#define ELFCLASS64  2

/* ELF data encoding */
#define ELFDATA2LSB 1

/* ELF type */
#define ET_EXEC     2

/* Machine type */
#define EM_X86_64   62

/* Program header type */
#define PT_NULL     0
#define PT_LOAD     1

/* Program header flags */
#define PF_X        (1 << 0)
#define PF_W        (1 << 1)
#define PF_R        (1 << 2)

/* ELF64 file header */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;       /* Program header table offset */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;       /* Number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

/* ELF64 program header */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;      /* Offset in file */
    uint64_t p_vaddr;       /* Virtual address in memory */
    uint64_t p_paddr;
    uint64_t p_filesz;      /* Size of segment in file */
    uint64_t p_memsz;       /* Size of segment in memory (>= filesz; extra is BSS) */
    uint64_t p_align;
} Elf64_Phdr;

/* Result of loading an ELF binary */
typedef struct {
    uint64_t entry_point;   /* Virtual address of ELF entry */
    uint64_t brk_start;     /* First address past last loaded segment (page-aligned) */
} ElfLoadResult;

/* Load an ELF64 binary into a process's address space.
 * data: pointer to ELF file in memory
 * size: size of ELF file
 * pml4_phys: physical address of process's PML4
 * result: output — entry point and break start
 * Returns 0 on success, negative error code on failure. */
int elf_load(const void *data, size_t size, uint64_t pml4_phys, ElfLoadResult *result);

#endif /* ARCHOS_PROC_ELF_H */
