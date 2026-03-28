#ifndef ARCHOS_LIBC_DIRENT_H
#define ARCHOS_LIBC_DIRENT_H

#include <stdint.h>

#define DT_REG  0   /* Regular file */
#define DT_DIR  1   /* Directory */

struct dirent {
    char     d_name[256];
    uint64_t d_ino;
    uint8_t  d_type;
};

typedef struct {
    int           fd;      /* Open directory fd (unused — we use path-based readdir) */
    char          path[256];
    struct dirent entries[64];
    int           count;
    int           pos;
} DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int            closedir(DIR *dir);

#endif /* ARCHOS_LIBC_DIRENT_H */
