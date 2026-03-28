#ifndef ARCHOS_LIBC_SYS_STAT_H
#define ARCHOS_LIBC_SYS_STAT_H

#include <stdint.h>
#include <sys/types.h>

/* Stat structure — matches kernel VfsStat layout */
struct stat {
    uint64_t st_ino;
    uint8_t  st_type;
    uint64_t st_size;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
};

/* Special mode bits */
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t owner, gid_t group);
int mkdir(const char *path, mode_t mode);

#endif /* ARCHOS_LIBC_SYS_STAT_H */
