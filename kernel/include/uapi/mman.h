#ifndef ARCHOS_UAPI_MMAN_H
#define ARCHOS_UAPI_MMAN_H

/* mmap, mprotect, madvise, msync and mremap values, Linux x86_64 (asm-generic/mman-common.h,
 * asm-generic/mman.h, asm/mman.h, linux/mman.h). Checked by test_abi_layout. */

#define PROT_NONE      0x0
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define PROT_SEM       0x8
#define PROT_GROWSDOWN 0x01000000
#define PROT_GROWSUP   0x02000000

#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_SHARED_VALIDATE 0x03
#define MAP_TYPE            0x0f
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_32BIT           0x40
#define MAP_GROWSDOWN       0x0100
#define MAP_DENYWRITE       0x0800
#define MAP_EXECUTABLE      0x1000
#define MAP_LOCKED          0x2000
#define MAP_NORESERVE       0x4000
#define MAP_POPULATE        0x008000
#define MAP_NONBLOCK        0x010000
#define MAP_STACK           0x020000
#define MAP_HUGETLB         0x040000
#define MAP_FIXED_NOREPLACE 0x100000

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
#define MADV_HUGEPAGE   14
#define MADV_NOHUGEPAGE 15

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
#define MREMAP_DONTUNMAP 4

#endif /* ARCHOS_UAPI_MMAN_H */
