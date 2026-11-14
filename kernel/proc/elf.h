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
#define ET_DYN      3       /* static-PIE when there is no PT_INTERP */

/* Machine type */
#define EM_X86_64   62

/* Program header type */
#define PT_NULL      0
#define PT_LOAD      1
#define PT_INTERP    3
#define PT_PHDR      6
#define PT_TLS       7
#define PT_GNU_STACK 0x6474E551

/* Static-PIE programs load at this fixed base (Linux's ELF_ET_DYN_BASE without
 * randomisation). Page-aligned and far below the stack and brk limit. */
#define ELF_PIE_BASE 0x0000555555554000ULL

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

/* Result of loading an ELF binary (user addresses include load_bias) */
typedef struct {
    uint64_t entry_point;   /* Virtual address of ELF entry */
    uint64_t brk_start;     /* First address past last loaded segment (page-aligned) */
    uint64_t load_bias;     /* 0 for ET_EXEC, ELF_PIE_BASE - lowest vaddr for static-PIE */
    uint64_t phdr_addr;     /* Program headers in user memory (AT_PHDR), 0 if not loaded */
    uint16_t phnum;         /* AT_PHNUM */
    uint16_t phent;         /* AT_PHENT */
    uint8_t  stack_exec;    /* PT_GNU_STACK asks for an executable stack (or is absent) */
} ElfLoadResult;

/* Linux's limit on the program header table (fs/binfmt_elf.c) */
#define ELF_MAX_PHDR_BYTES 65536

struct Mm;
struct VmaOps;

/* Where elf_load_from reads a program: `read` copies bytes at an offset (0
 * or -errno). With `ops` set (a file whose data is in the page cache), whole
 * pages of segment data are mapped from `file` through ops instead of copied
 * (M1-12). */
typedef struct ElfSource {
    uint64_t size;   /* bytes */
    int (*read)(const struct ElfSource *src, uint64_t off, void *dst, size_t len);
    const void *data;             /* memory source */
    void *file;                   /* file source: a File (the VMAs take their own references) */
    const struct VmaOps *ops;     /* NULL: copy every page */
} ElfSource;

/* A source reading from memory (no mapping). */
void elf_source_mem(ElfSource *src, const void *data, uint64_t size);

/* Load a static ELF64 executable (ET_EXEC, or ET_DYN without PT_INTERP) into
 * a process's address space (ADR-0002). Each PT_LOAD page gets its segment's
 * permissions and VMA (M1-07); a page two segments share gets both (its own
 * one-page VMA). From a file source, a segment's whole pages of file data are
 * a private file VMA (read on first touch, copied on first write); the page
 * a previous segment shares and the last, partly-file page are copied now;
 * .bss pages come on first touch. What it mapped before a failure stays: the
 * caller frees the mm.
 * mm: the new address space (nobody else can reach it yet)
 * result: output — entry point and break start
 * Returns 0, -ENOEXEC for a file that is not a static x86_64 ELF executable
 * (including any program with PT_INTERP), -ENOMEM, or the source's read error. */
int elf_load_from(const ElfSource *src, struct Mm *mm, ElfLoadResult *result);

/* elf_load_from with a memory source: data, size bytes. */
int elf_load(const void *data, size_t size, struct Mm *mm, ElfLoadResult *result);

#endif /* ARCHOS_PROC_ELF_H */
