#ifndef ARCHOS_LIB_CMDLINE_H
#define ARCHOS_LIB_CMDLINE_H

#include <stddef.h>

/* Maximum stored kernel command line length, including the NUL. */
#define CMDLINE_MAX 256

/* Store a copy of the kernel command line (truncated to CMDLINE_MAX - 1).
 * NULL stores an empty line. Called once at boot. */
void cmdline_set(const char *line);

/* The stored kernel command line; never NULL (empty if none was given). */
const char *cmdline_raw(void);

/* Look up `key` in a space-separated "key=value key2 ..." command line.
 * Copies the value (empty for a bare "key") into out[0..outsz-1], NUL-terminated
 * and truncated if needed. Returns 1 if the key is present, 0 if not.
 * out may be NULL when only presence matters. */
int cmdline_get(const char *line, const char *key, char *out, size_t outsz);

#endif /* ARCHOS_LIB_CMDLINE_H */
