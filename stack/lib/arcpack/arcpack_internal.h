/*
 * arcpack_internal.h — what the arcpack reader and writer share: file names, magics and record
 * sizes (pack-v1.md). Not for callers outside stack/lib/arcpack/. Name and version rules come
 * from arcjson (arcjson_is_schema_id, arcjson_is_schema_version).
 */
#ifndef ARCHOS_STACK_ARCPACK_INTERNAL_H
#define ARCHOS_STACK_ARCPACK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "arcpack.h"

/* The pack's binary files, relative to the pack directory. */
enum { ARCPACK_FILE_DOCS, ARCPACK_FILE_STRINGS, ARCPACK_FILE_ZST, ARCPACK_FILE_CHUNKS, ARCPACK_NFILES };

typedef struct {
    const char *path;
    char magic[8];
    unsigned record_size;
} arcpack_file_kind;

/* Indexed by ARCPACK_FILE_*. */
extern const arcpack_file_kind arcpack_files[ARCPACK_NFILES];

#endif /* ARCHOS_STACK_ARCPACK_INTERNAL_H */
