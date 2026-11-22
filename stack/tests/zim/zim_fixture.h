/*
 * zim_fixture — writes small ZIM files for tests (M3-03a). Test-only: lane tests never download a ZIM
 * (content-lane.md), so they build their own with this writer, and patch its bytes to make damaged ones.
 *
 * Layout written: header, MIME list, clusters (in cluster order), dirents (in path order), path pointer
 * list, title pointer list (if asked), cluster pointer list, 16-byte checksum. The checksum is zero:
 * arczim doesn't verify MD5 (ADR-0022), so tests don't need it.
 */
#ifndef ARCHOS_STACK_TESTS_ZIM_FIXTURE_H
#define ARCHOS_STACK_TESTS_ZIM_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One entry. Content entries name a MIME type and a cluster; redirects name their target as
 * "<ns>/<path>"; `title` NULL or "" means "same as the path". */
typedef struct {
    char ns;
    const char *path;
    const char *title;
    const char *mime;     /* content entries */
    const char *data;     /* content entries */
    size_t size;
    unsigned cluster;     /* content entries: index into zimfx_spec.clusters */
    const char *redirect; /* redirect entries: "C/Target" */
} zimfx_entry;

typedef struct {
    uint8_t compression; /* ARCZIM_COMP_NONE or ARCZIM_COMP_ZSTD (others: the bytes are written raw) */
    bool extended;
} zimfx_cluster;

typedef struct {
    uint16_t major, minor;
    const zimfx_entry *entries;
    size_t n_entries;
    const zimfx_cluster *clusters;
    size_t n_clusters;
    bool title_list;       /* write a title pointer list (else titlePtrPos = all ones) */
    const char *main_page; /* "<ns>/<path>" or NULL */
} zimfx_spec;

/* Where things landed, for tests that patch the file. Arrays are malloc'd (zimfx_layout_free). */
typedef struct {
    uint64_t mime_list_pos, path_ptr_pos, title_ptr_pos, cluster_ptr_pos, checksum_pos;
    uint64_t *dirent_pos;  /* by entry number (path order) */
    uint32_t *entry_of;    /* spec index -> entry number */
    uint64_t *cluster_pos; /* by cluster number */
} zimfx_layout;

/*
 * zimfx_build: the ZIM for `spec` in a malloc'd buffer (*out, *len); `lay` may be NULL. False with a
 * message in err if the spec is inconsistent (unknown redirect target, repeated path, bad cluster).
 */
bool zimfx_build(const zimfx_spec *spec, uint8_t **out, size_t *len, zimfx_layout *lay, char *err, size_t errlen);

/* zimfx_layout_free: free the arrays of a layout. */
void zimfx_layout_free(zimfx_layout *lay);

/* zimfx_write_file: write buf to path (replacing it); false on error. */
bool zimfx_write_file(const char *path, const uint8_t *buf, size_t len);

#endif /* ARCHOS_STACK_TESTS_ZIM_FIXTURE_H */
