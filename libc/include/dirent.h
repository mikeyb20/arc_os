#ifndef ARCHOS_LIBC_DIRENT_H
#define ARCHOS_LIBC_DIRENT_H

#include <stdint.h>
#include <sys/types.h>

/* DT_* values (Linux, ADR-0001) */
#include <uapi/dirent.h>

/* One entry as readdir returns it: the getdents64 record, read in place. */
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;           /* DT_* */
    char     d_name[256];
};

#define ARC_DIRBUF_SIZE 2048

/* Directory stream: an open directory fd and one getdents64 buffer. */
typedef struct {
    int      fd;
    int      pos;              /* next record in buf */
    int      len;              /* bytes of records in buf */
    char     buf[ARC_DIRBUF_SIZE] __attribute__((aligned(8)));
} DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int            closedir(DIR *dir);

#endif /* ARCHOS_LIBC_DIRENT_H */
