#ifndef ARCHOS_LIBC_FCNTL_H
#define ARCHOS_LIBC_FCNTL_H

/* Open flags — must match kernel/fs/vfs.h */
#define O_RDONLY   0x00
#define O_WRONLY   0x01
#define O_RDWR     0x02
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_ACCMODE  0x03

int open(const char *path, int flags, ...);

#endif /* ARCHOS_LIBC_FCNTL_H */
