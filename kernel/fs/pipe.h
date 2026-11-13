#ifndef ARCHOS_FS_PIPE_H
#define ARCHOS_FS_PIPE_H

#include "fs/vfs.h"

/* Pipe buffer size (bytes); writes up to this size are not interleaved with
 * other writers' (POSIX PIPE_BUF) */
#define PIPE_BUF_SIZE 4096

/* Create a pipe: *rd is its read end (O_RDONLY), *wr its write end
 * (O_WRONLY), each a File with one reference; extra_flags (O_NONBLOCK) go on
 * both. 0, or -ENOMEM. */
int pipe_create(File **rd, File **wr, uint32_t extra_flags);

#endif /* ARCHOS_FS_PIPE_H */
