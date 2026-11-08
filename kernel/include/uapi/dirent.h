#ifndef ARCHOS_UAPI_DIRENT_H
#define ARCHOS_UAPI_DIRENT_H

#include <stdint.h>

/* One record of the getdents64 buffer (ADR-0001). Records are packed back to
 * back; d_reclen is the full record length, a multiple of 8; d_name is
 * NUL-terminated; d_off is an opaque cookie for the next record. */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;           /* DT_* */
    char     d_name[];
};

_Static_assert(__builtin_offsetof(struct linux_dirent64, d_off) == 8, "d_off offset");
_Static_assert(__builtin_offsetof(struct linux_dirent64, d_reclen) == 16, "d_reclen offset");
_Static_assert(__builtin_offsetof(struct linux_dirent64, d_type) == 18, "d_type offset");
_Static_assert(__builtin_offsetof(struct linux_dirent64, d_name) == 19, "d_name offset");

/* d_type values */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

#endif /* ARCHOS_UAPI_DIRENT_H */
