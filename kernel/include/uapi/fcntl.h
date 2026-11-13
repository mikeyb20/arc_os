#ifndef ARCHOS_UAPI_FCNTL_H
#define ARCHOS_UAPI_FCNTL_H

/* open(2) flags, fcntl(2) commands and *at() flags, Linux x86_64 values
 * (ADR-0001). Since M1-11 the kernel honours every open flag here except
 * O_NOCTTY (accepted, no controlling-terminal rules yet). */

#define O_ACCMODE   00000003
#define O_RDONLY    00000000
#define O_WRONLY    00000001
#define O_RDWR      00000002
#define O_CREAT     00000100
#define O_EXCL      00000200
#define O_NOCTTY    00000400
#define O_TRUNC     00001000
#define O_APPEND    00002000
#define O_NONBLOCK  00004000
#define O_DIRECTORY 00200000
#define O_CLOEXEC   02000000

#define AT_FDCWD    (-100)

/* *at() flags */
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EMPTY_PATH       0x1000

/* fcntl commands */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

/* fd flags (F_GETFD / F_SETFD) */
#define FD_CLOEXEC 1

#endif /* ARCHOS_UAPI_FCNTL_H */
