#ifndef ARCHOS_LIBC_ERRNO_H
#define ARCHOS_LIBC_ERRNO_H

/* Error codes — must match kernel/fs/vfs.h */
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EIO          5
#define E2BIG        7
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENOSPC      28
#define ESPIPE      29
#define EPIPE       32
#define ENAMETOOLONG 36
#define ENOSYS      38
#define ENOTEMPTY   39

extern int errno;

#endif /* ARCHOS_LIBC_ERRNO_H */
