#ifndef ARCHOS_UAPI_AUXVEC_H
#define ARCHOS_UAPI_AUXVEC_H

/* Auxiliary vector keys (System V process start, ADR-0002), Linux values
 * (linux/auxvec.h). execve puts these pairs after envp's NULL. */

#define AT_NULL        0    /* end of vector */
#define AT_IGNORE      1
#define AT_EXECFD      2
#define AT_PHDR        3    /* program headers in memory */
#define AT_PHENT       4    /* size of one program header */
#define AT_PHNUM       5    /* number of program headers */
#define AT_PAGESZ      6
#define AT_BASE        7    /* interpreter base; 0 for static programs */
#define AT_FLAGS       8
#define AT_ENTRY       9    /* program entry point */
#define AT_NOTELF      10
#define AT_UID         11
#define AT_EUID        12
#define AT_GID         13
#define AT_EGID        14
#define AT_PLATFORM    15   /* string, "x86_64" */
#define AT_HWCAP       16   /* CPUID leaf 1 EDX */
#define AT_CLKTCK      17
#define AT_SECURE      23
#define AT_RANDOM      25   /* address of 16 random bytes */
#define AT_HWCAP2      26
#define AT_EXECFN      31   /* filename passed to execve */
#define AT_MINSIGSTKSZ 51

#endif /* ARCHOS_UAPI_AUXVEC_H */
