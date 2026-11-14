#ifndef ARCHOS_FS_VFS_INTERNAL_H
#define ARCHOS_FS_VFS_INTERNAL_H

/* VFS internals shared by inode.c, dcache.c, vfs.c and file.c (M1-11).
 * Filesystems use vfs.h only. */

#include "fs/vfs.h"

/* Most entries the dentry cache keeps; the least recently used go first. */
#define DCACHE_MAX 4096

/* Slab caches and tables of file.c and dcache.c (called by vfs_init). */
void vfs_file_init(void);
void dcache_init(void);

/* The child called name in dir, with a new reference, or NULL when the cache
 * doesn't know it (the caller then asks the filesystem). */
Inode *dcache_lookup(Inode *dir, const char *name);

/* Remember dir/name -> child. The entry holds references to both inodes. Call
 * with dir locked (shared is enough). Does nothing if out of memory or if the
 * name is already cached. */
void dcache_add(Inode *dir, const char *name, Inode *child);

/* Forget dir/name. Call with dir locked exclusive, before the filesystem
 * changes the entry. */
void dcache_remove(Inode *dir, const char *name);

/* Create name in dir (locked exclusive) as a regular file with mode less the
 * caller's umask, owned by the caller. *out gets a reference. */
int vfs_create_locked(Inode *dir, const char *name, uint32_t mode, Inode **out);

/* The inode open() works on: path resolved from base (symlinks followed
 * unless O_NOFOLLOW, which makes a final one -ELOOP), or with O_CREAT
 * created when missing (*created = 1) or refused when present with O_EXCL. */
int vfs_open_locate(const PathLoc *base, const char *path, uint32_t flags, uint32_t mode, PathLoc *out, int *created);

/* Entries cached now (tests, /proc/meminfo-style counters). */
uint32_t dcache_count(void);

#endif /* ARCHOS_FS_VFS_INTERNAL_H */
