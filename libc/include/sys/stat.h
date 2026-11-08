#ifndef ARCHOS_LIBC_STAT_H
#define ARCHOS_LIBC_STAT_H

#include <stdint.h>
#include <sys/types.h>

/* struct stat (Linux x86_64 layout), S_IF* type bits and S_IS* tests (ADR-0001) */
#include <uapi/stat.h>

int    stat(const char *path, struct stat *buf);
int    fstat(int fd, struct stat *buf);
int    chmod(const char *path, mode_t mode);
int    chown(const char *path, uid_t owner, gid_t group);
int    mkdir(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#endif /* ARCHOS_LIBC_STAT_H */
