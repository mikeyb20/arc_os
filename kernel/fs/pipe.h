#ifndef ARCHOS_FS_PIPE_H
#define ARCHOS_FS_PIPE_H

#include "fs/vfs.h"

/* Pipe buffer size (bytes) */
#define PIPE_BUF_SIZE 4096

/* Create a pipe. Sets *read_node and *write_node to the two ends.
 * Returns 0 on success, -ENOMEM on failure. */
int pipe_create(VfsNode **read_node, VfsNode **write_node);

/* Close one end of a pipe. Decrements the appropriate ref count.
 * Frees the PipeNode when both ends reach 0. */
void pipe_close(VfsNode *node);

/* Add a reference to one end of a pipe (used by dup2/fork). */
void pipe_addref(VfsNode *node);

#endif /* ARCHOS_FS_PIPE_H */
