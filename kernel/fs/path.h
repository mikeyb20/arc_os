#ifndef ARCHOS_FS_PATH_H
#define ARCHOS_FS_PATH_H

#include <stddef.h>

/* Maximum length of a full path (including NUL) */
#define PATH_MAX 512

/* Resolve input (relative or absolute) against cwd into a normalized absolute path.
 * Handles ".", "..", collapsed "//", trailing "/".
 * Returns 0 on success, -ENAMETOOLONG or -EINVAL on error. */
int path_normalize(const char *cwd, const char *input, char *out, size_t out_size);

#endif /* ARCHOS_FS_PATH_H */
