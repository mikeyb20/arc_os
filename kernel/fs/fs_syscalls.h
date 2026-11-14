#ifndef ARCHOS_FS_FS_SYSCALLS_H
#define ARCHOS_FS_FS_SYSCALLS_H

/* Register the file and directory syscalls under their Linux numbers: read,
 * write, readv, writev, pread64, pwrite64, open, openat, close, stat, lstat,
 * fstat, newfstatat, lseek, ftruncate, fsync, ioctl, fcntl, pipe, pipe2, dup,
 * dup2, dup3, getcwd, chdir, mkdir, mkdirat, rmdir, unlink, unlinkat,
 * rename, renameat, readlink, readlinkat, chmod, chown, umask, getdents64.
 * Called by syscall_init. */
void fs_syscalls_init(void);

#endif /* ARCHOS_FS_FS_SYSCALLS_H */
