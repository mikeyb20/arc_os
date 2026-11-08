#ifndef ARCHOS_UAPI_STAT_H
#define ARCHOS_UAPI_STAT_H

#include <stdint.h>

/* Linux x86_64 `struct stat` (stat, fstat, newfstatat) and mode bits (ADR-0001).
 * Checked against the host's <asm/stat.h> by tests/test_abi_layout.c. */

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;          /* S_IF* type bits | permission bits */
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;        /* 512-byte blocks */
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

_Static_assert(sizeof(struct stat) == 144, "struct stat must be 144 bytes");
_Static_assert(__builtin_offsetof(struct stat, st_nlink) == 16, "st_nlink offset");
_Static_assert(__builtin_offsetof(struct stat, st_mode) == 24, "st_mode offset");
_Static_assert(__builtin_offsetof(struct stat, st_uid) == 28, "st_uid offset");
_Static_assert(__builtin_offsetof(struct stat, st_size) == 48, "st_size offset");
_Static_assert(__builtin_offsetof(struct stat, st_blocks) == 64, "st_blocks offset");
_Static_assert(__builtin_offsetof(struct stat, st_mtime) == 88, "st_mtime offset");

/* File type bits in st_mode */
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Special and permission bits */
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000

#endif /* ARCHOS_UAPI_STAT_H */
