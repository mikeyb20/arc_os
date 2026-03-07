#ifndef ARCHOS_PROC_FD_H
#define ARCHOS_PROC_FD_H

#include <stdint.h>
#include "fs/vfs.h"

/* Maximum file descriptors per process */
#define MAX_FDS 64

/* File descriptor entry */
typedef struct {
    VfsFile file;
    uint8_t in_use;
} FdEntry;

/* Per-process file descriptor table */
typedef struct FdTable {
    FdEntry entries[MAX_FDS];
} FdTable;

/* Initialize a file descriptor table (all entries unused). */
void fd_table_init(FdTable *table);

/* Allocate the lowest free fd. Returns fd number, or -1 if full. */
int fd_alloc(FdTable *table);

/* Free a file descriptor. */
void fd_free(FdTable *table, int fd);

/* Get the VfsFile for a file descriptor. Returns NULL if invalid/unused. */
VfsFile *fd_get(FdTable *table, int fd);

#endif /* ARCHOS_PROC_FD_H */
