#ifndef ARCHOS_FS_FS_SYSCALLS_H
#define ARCHOS_FS_FS_SYSCALLS_H

#include <stddef.h>
#include <stdint.h>

/* Register the file and directory syscalls under their Linux numbers: read,
 * write, readv, writev, pread64, pwrite64, open, openat, close, stat, lstat,
 * fstat, newfstatat, lseek, ftruncate, fsync, ioctl, fcntl, pipe, pipe2, dup,
 * dup2, dup3, getcwd, chdir, mkdir, mkdirat, rmdir, unlink, unlinkat,
 * rename, renameat, chmod, chown, umask, getdents64. Called by syscall_init. */
void fs_syscalls_init(void);

/* Turn a user-space path into an absolute, normalized path using the calling
 * process's cwd. Returns 0, -EFAULT for a bad pointer, or a path error. */
int fs_user_path(uint64_t path_addr, char *abs, size_t size);

#endif /* ARCHOS_FS_FS_SYSCALLS_H */
