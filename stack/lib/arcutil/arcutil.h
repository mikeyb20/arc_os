/*
 * arcutil — small helpers every stack program shares (M3-06a, M5-02; ADR-0020: nothing else in stack/
 * carries its own copy).
 *
 * - SHA-256 (FIPS 180-4), streaming: package and file verification.
 * - Durable files (lane-l.md, "Durable updates"): write a temporary file in the same directory,
 *   fsync it, rename it into place, fsync the directory. Directory creation and fsync.
 * - Recursive directory removal.
 * - Total RAM, for the model fit checks.
 *
 * No state; every function sets errno on failure.
 */
#ifndef ARCHOS_STACK_ARCUTIL_H
#define ARCHOS_STACK_ARCUTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- SHA-256 ------------------------------------------------------------------------------------ */

#define ARCUTIL_SHA256_SIZE 32
#define ARCUTIL_SHA256_HEX  65 /* 64 lowercase hex digits and a NUL */

typedef struct {
    uint32_t h[8];
    uint64_t length; /* bytes hashed so far */
    unsigned char block[64];
    size_t used; /* bytes in block */
} arcutil_sha256;

void arcutil_sha256_init(arcutil_sha256 *s);
void arcutil_sha256_update(arcutil_sha256 *s, const void *data, size_t len);
void arcutil_sha256_final(arcutil_sha256 *s, unsigned char out[ARCUTIL_SHA256_SIZE]);

/* arcutil_sha256_hex: digest as 64 lowercase hex digits and a NUL. */
void arcutil_sha256_hex(const unsigned char digest[ARCUTIL_SHA256_SIZE], char out[ARCUTIL_SHA256_HEX]);

/* ---- Files and directories ---------------------------------------------------------------------- */

/* arcutil_make_dir: mkdir `path` (0755), also true if it already exists as a directory. */
bool arcutil_make_dir(const char *path);

/* arcutil_make_dirs: arcutil_make_dir for `path` and every missing parent ("mkdir -p"). */
bool arcutil_make_dirs(const char *path);

/* arcutil_sync_dir: fsync directory `path`, so renames and new names in it are durable. */
bool arcutil_sync_dir(const char *path);

/*
 * arcutil_write_durable: replace the file at `path` with data[0..len): write `path`.tmp, fsync,
 * rename over `path`, fsync the directory. Readers see the old or the new file, never a mix.
 */
bool arcutil_write_durable(const char *path, const void *data, size_t len);

/*
 * arcutil_remove_tree: remove `path` and, if it is a directory, everything under it (symlinks are
 * removed, never followed). True if `path` is gone, including when it never existed.
 */
bool arcutil_remove_tree(const char *path);

/* ---- System ------------------------------------------------------------------------------------- */

/* arcutil_ram_mib: total RAM in MiB (sysinfo), 0 if unknown. */
uint64_t arcutil_ram_mib(void);

#endif /* ARCHOS_STACK_ARCUTIL_H */
