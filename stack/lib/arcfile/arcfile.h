/*
 * arcfile — the stack's durable-file helpers (lane-l.md, "Durable updates": write a temporary file in
 * the same directory, fsync it, rename it into place, fsync the directory). Used by every stack
 * writer: the pack writer (M3-02), the pack index builder (M3-06a).
 */
#ifndef ARCHOS_STACK_ARCFILE_H
#define ARCHOS_STACK_ARCFILE_H

#include <stdbool.h>

/* arcfile_make_dir: mkdir `path` (0755), also true if it already exists as a directory; false with errno. */
bool arcfile_make_dir(const char *path);

/* arcfile_sync_dir: fsync directory `path`, so renames and new names in it are durable; false with errno. */
bool arcfile_sync_dir(const char *path);

#endif /* ARCHOS_STACK_ARCFILE_H */
