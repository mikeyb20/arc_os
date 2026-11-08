#include "proc/fd.h"
#include "lib/mem.h"
#include "mm/kmalloc.h"
#include "fs/pipe.h"
#include "net/socket.h"

/* A socket fd has no VfsNode: the socket index is in offset (net_syscalls.c) */
static int fd_file_socket(const VfsFile *f) {
    return (f->node == NULL && f->flags == VFS_SOCKET) ? (int)f->offset : -1;
}

void fd_file_addref(const VfsFile *f) {
    if (f->node != NULL && f->node->type == VFS_PIPE) {
        pipe_addref(f->node);
    } else if (fd_file_socket(f) >= 0) {
        socket_addref(fd_file_socket(f));
    }
}

void fd_close(FdTable *table, int fd) {
    VfsFile *f = fd_get(table, fd);
    if (f == NULL) return;
    if (f->node != NULL && f->node->type == VFS_PIPE) {
        pipe_close(f->node);
    } else if (fd_file_socket(f) >= 0) {
        socket_release(fd_file_socket(f));
    }
    vfs_close(f);
    fd_free(table, fd);
}

void fd_table_init(FdTable *table) {
    memset(table, 0, sizeof(FdTable));
}

int fd_alloc(FdTable *table) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!table->entries[i].in_use) {
            table->entries[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

void fd_free(FdTable *table, int fd) {
    if (fd >= 0 && fd < MAX_FDS) {
        table->entries[fd].in_use = 0;
    }
}

VfsFile *fd_get(FdTable *table, int fd) {
    if (fd < 0 || fd >= MAX_FDS || !table->entries[fd].in_use) {
        return NULL;
    }
    return &table->entries[fd].file;
}

FdTable *fd_table_dup(const FdTable *src) {
    if (src == NULL) return NULL;
    FdTable *dst = kmalloc(sizeof(FdTable), 0);
    if (dst == NULL) return NULL;
    memcpy(dst, src, sizeof(FdTable));

    for (int i = 0; i < MAX_FDS; i++) {
        if (dst->entries[i].in_use) fd_file_addref(&dst->entries[i].file);
    }
    return dst;
}
