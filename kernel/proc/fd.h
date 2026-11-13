#ifndef ARCHOS_PROC_FD_H
#define ARCHOS_PROC_FD_H

/* File descriptor tables (M1-11).
 *
 * An fd is a slot holding a File reference plus the close-on-exec flag. Two
 * fds (dup) and two processes (fork) share one File, and so its offset and
 * status flags. A table is refcounted so the threads of one process can
 * share it (M1-16).
 *
 * Locks: FdTable.lock (LOCK_LEVEL_FD_TABLE, spinlock) guards the slots and
 * bitmaps; nothing but atomic File refcounts is touched under it. A File
 * leaving a slot is released (fput) after the lock is dropped. fd_get hands
 * out a reference, so a File stays valid while a syscall uses it even if
 * another thread closes the fd meanwhile. */

#include <stdint.h>
#include "fs/vfs.h"
#include "proc/spinlock.h"

/* File descriptors per process (fits the 64-bit slot bitmaps) */
#define MAX_FDS 64

typedef struct FdTable {
    Spinlock lock;
    uint32_t refcount;        /* atomic: owners sharing the table */
    uint64_t open;            /* bit n: fd n holds a File */
    uint64_t cloexec;         /* bit n: close fd n on execve */
    File *files[MAX_FDS];
} FdTable;
_Static_assert(MAX_FDS <= 64, "fd bitmaps are one uint64_t");

/* New empty table with one owner. NULL when out of memory. */
FdTable *fd_table_new(void);

/* Copy for fork: every open fd shares its File with the original (one more
 * reference each), close-on-exec flags kept. NULL when out of memory. */
FdTable *fd_table_dup(FdTable *src);

/* Take / drop an owner. The last drop closes every fd and frees the table. */
void fd_table_get(FdTable *t);
void fd_table_put(FdTable *t);

/* Install file (the caller's reference passes to the table) in the lowest
 * free fd >= min_fd. Returns the fd, or -EMFILE (the reference is then
 * still the caller's), or -EINVAL for min_fd out of range. */
int fd_install(FdTable *t, File *file, int min_fd, int cloexec);

/* Install file at exactly fd, closing what was there (dup2/dup3). Takes over
 * the caller's reference. 0 or -EBADF. */
int fd_install_at(FdTable *t, int fd, File *file, int cloexec);

/* The File at fd with a new reference (fput it), or NULL. */
File *fd_get(FdTable *t, int fd);

/* Close fd. 0 or -EBADF. */
int fd_close(FdTable *t, int fd);

/* Close-on-exec flag of fd: get (0/1, or -EBADF) and set (0 or -EBADF). */
int fd_get_cloexec(FdTable *t, int fd);
int fd_set_cloexec(FdTable *t, int fd, int on);

/* Close every fd marked close-on-exec (execve). */
void fd_close_on_exec(FdTable *t);

/* Close every fd (exit: pipe readers then see EOF). */
void fd_close_all(FdTable *t);

#endif /* ARCHOS_PROC_FD_H */
