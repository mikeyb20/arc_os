/*
 * arczim — our own reader for Kiwix ZIM archives (M3-03a, ADR-0022). Format as we read it:
 * docs/formats/zim.md (openZIM's spec pages are archived as `zim-spec` in third_party/SOURCES.lock).
 *
 * Build host only. The pack pipeline reads ZIMs on Linux (ADR-0011: arc_os never parses a source
 * format), so nothing that runs on arc_os links this library; only `zimcat` and the tests do.
 *
 * arczim_open() maps the file read-only and checks the header, the MIME list and that the pointer
 * lists lie inside the file. Every directory entry, cluster and blob is checked when it is read, so
 * a damaged file gives an error, never a crash or an out-of-bounds read. arczim_check() walks the
 * whole file once and checks everything (and gathers the numbers `zimcat check` prints).
 *
 * Clusters: uncompressed (0, 1) are read in place; zstd (5) is decompressed into a small LRU cache of
 * cluster buffers (window up to 2^27, size up to ARCZIM_CLUSTER_MAX_DEFAULT). xz (4), zlib (2) and
 * bzip2 (3) are refused with an error naming the cluster: every ZIM on the content list is zstd, and
 * supporting xz would mean archiving xz-utils (ADR-0022).
 *
 * Cost: opening is O(1) in the file size plus the MIME list. Entry lookup by index is O(1); by path
 * O(log entries) dirent reads. Reading a blob from a cached cluster is O(1); a cache miss decompresses
 * one cluster (about 1-2 MiB of text in Kiwix ZIMs). Memory: the cache slots (default 4 clusters).
 * arczim_check: 8 bytes per content entry and 16 per cluster, plus one cluster buffer.
 */
#ifndef ARCHOS_STACK_ARCZIM_H
#define ARCHOS_STACK_ARCZIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARCZIM_MAGIC        72173914u /* 0x044D495A, "ZIM\x04" */
#define ARCZIM_HEADER_SIZE  80u
#define ARCZIM_NO_ENTRY     0xFFFFFFFFu /* header mainPage / layoutPage: none */
#define ARCZIM_NO_TITLE_PTR UINT64_MAX  /* header titlePtrPos: no title list (minor >= 3) */

/* Dirent MIME numbers that are not indexes into the MIME list. */
#define ARCZIM_MIME_REDIRECT   0xFFFFu
#define ARCZIM_MIME_LINKTARGET 0xFFFEu /* deprecated */
#define ARCZIM_MIME_DELETED    0xFFFDu /* deprecated */

/* Cluster info byte: low 4 bits compression, bit 4 extended (8-byte blob offsets). */
#define ARCZIM_COMP_NONE_OLD    0u
#define ARCZIM_COMP_NONE        1u
#define ARCZIM_COMP_ZLIB        2u /* removed from the format */
#define ARCZIM_COMP_BZIP2       3u /* removed from the format */
#define ARCZIM_COMP_XZ          4u /* refused (ADR-0022) */
#define ARCZIM_COMP_ZSTD        5u
#define ARCZIM_CLUSTER_EXTENDED 0x10u

/* Limits that keep a damaged file from costing much. */
#define ARCZIM_CLUSTER_MAX_DEFAULT (512u << 20) /* decompressed bytes of one cluster */
#define ARCZIM_ZSTD_WINDOW_LOG_MAX 27           /* 128 MiB; libzim's level-19 frames use 2^23 */
#define ARCZIM_STRING_MAX          65536u       /* bytes of a path, title or MIME type */
#define ARCZIM_MIME_MAX            0xFFFDu      /* MIME list entries (higher numbers are reserved) */
#define ARCZIM_REDIRECT_HOPS_MAX   32u
#define ARCZIM_CACHE_SLOTS_DEFAULT 4u
#define ARCZIM_CACHE_SLOTS_MAX     64u

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "ZIM integers are little-endian, read in place");

typedef struct arczim arczim;

/* The header fields, as read (docs/formats/zim.md, "Header"). */
typedef struct {
    uint16_t major;
    uint16_t minor;
    uint8_t uuid[16];
    uint32_t entry_count;
    uint32_t cluster_count;
    uint64_t path_ptr_pos;
    uint64_t title_ptr_pos;
    uint64_t cluster_ptr_pos;
    uint64_t mime_list_pos;
    uint32_t main_page;
    uint32_t layout_page;
    uint64_t checksum_pos;
} arczim_header;

typedef enum {
    ARCZIM_ENTRY_CONTENT,  /* data in cluster/blob */
    ARCZIM_ENTRY_REDIRECT, /* points to another entry */
    ARCZIM_ENTRY_OBSOLETE, /* deprecated link target or deleted entry: no data, skip it */
} arczim_entry_kind;

/* One directory entry, checked. `path` and `title` point into the mapped file; both are also
 * NUL-terminated there. A title that is empty in the file is reported as the path. */
typedef struct {
    uint32_t index; /* entry number = rank in (namespace, path) order */
    arczim_entry_kind kind;
    char ns;           /* namespace: 'C' content, 'M' metadata, 'W' well-known, 'X' indexes; old: 'A', 'I', ... */
    uint16_t mime;     /* MIME list index (content entries) */
    uint32_t cluster;  /* content entries */
    uint32_t blob;     /* content entries */
    uint32_t redirect; /* redirect entries: target entry number */
    const char *path;
    size_t path_length;
    const char *title;
    size_t title_length;
} arczim_entry;

/* One cluster, as described by its pointer and info byte (no decompression). */
typedef struct {
    uint64_t offset;     /* of the info byte */
    uint8_t compression; /* ARCZIM_COMP_* */
    bool extended;
} arczim_cluster;

/*
 * arczim_open: open and map the ZIM at `path`. Returns NULL with *why set (a static or per-thread
 * message, valid until the next arczim_open on this thread) if the file can't be read or its
 * header, MIME list or pointer lists are wrong.
 */
arczim *arczim_open(const char *path, const char **why);

/* arczim_close: unmap and free; NULL is fine. */
void arczim_close(arczim *z);

/* arczim_error: why the last failed call on `z` failed. */
const char *arczim_error(const arczim *z);

/* arczim_set_cache_slots: number of decompressed clusters kept (1..ARCZIM_CACHE_SLOTS_MAX); drops the cache. */
bool arczim_set_cache_slots(arczim *z, unsigned slots);

/* arczim_set_cluster_max: largest decompressed cluster accepted, in bytes (>= 64); drops the cache. */
bool arczim_set_cluster_max(arczim *z, size_t bytes);

/* arczim_get_header: the header as read. */
const arczim_header *arczim_get_header(const arczim *z);

/* arczim_file_size: bytes in the file. */
uint64_t arczim_file_size(const arczim *z);

/* Entry and cluster counts (from the header). */
uint32_t arczim_entry_count(const arczim *z);
uint32_t arczim_cluster_count(const arczim *z);

/* MIME list: count, and type `i` (NUL-terminated, in the mapping); NULL if i >= count. */
uint16_t arczim_mime_count(const arczim *z);
const char *arczim_mime(const arczim *z, uint16_t i);

/* arczim_entry_get: entry number `index`, checked; false (see arczim_error) if out of range or damaged. */
bool arczim_entry_get(arczim *z, uint32_t index, arczim_entry *out);

/*
 * arczim_lower_bound: number of the first entry whose (ns, path) is not below (ns, path[0..len)), or
 * arczim_entry_count() if none: where a namespace or a path prefix starts. O(log entries).
 */
bool arczim_lower_bound(arczim *z, char ns, const char *path, size_t len, uint32_t *index);

/*
 * arczim_find: binary search for the entry (ns, path[0..len)); *index = its number. False if not
 * found (arczim_error says "not found") or a dirent on the way is damaged. Relies on the path order
 * the format requires; arczim_check verifies it.
 */
bool arczim_find(arczim *z, char ns, const char *path, size_t len, uint32_t *index);

/*
 * arczim_resolve: follow redirects from entry `index` to a content entry (at most
 * ARCZIM_REDIRECT_HOPS_MAX hops; a loop is an error) and return it in *out.
 */
bool arczim_resolve(arczim *z, uint32_t index, arczim_entry *out);

/* arczim_cluster_get: cluster `n`'s offset and info byte. */
bool arczim_cluster_get(arczim *z, uint32_t n, arczim_cluster *out);

/*
 * arczim_blob: blob `blob` of cluster `cluster`: *data points into the mapping (uncompressed
 * clusters) or into the cluster cache. Valid until the next arczim_blob, arczim_content,
 * arczim_metadata or arczim_check call, or a cache setting change, on `z`.
 */
bool arczim_blob(arczim *z, uint32_t cluster, uint32_t blob, const char **data, uint64_t *size);

/* arczim_content: the data of content entry `e` (same lifetime as arczim_blob). False for redirects. */
bool arczim_content(arczim *z, const arczim_entry *e, const char **data, uint64_t *size);

/*
 * arczim_metadata: value of metadata key `key` (entry M/<key>, redirects followed), same lifetime as
 * arczim_blob. False if missing (arczim_error says "not found") or damaged.
 */
bool arczim_metadata(arczim *z, const char *key, const char **value, uint64_t *size);

/* ---- Whole-file check (zimcat check) -------------------------------------------------------- */

/* Path-order cluster locality: cluster decompressions needed to read every content entry of
 * namespace `ns` with text/html MIME in path order, through an LRU cache of each size. */
#define ARCZIM_LRU_SIZES 4
static const unsigned arczim_lru_sizes[ARCZIM_LRU_SIZES] = {1, 4, 16, 64};

typedef struct {
    uint64_t entries;         /* per MIME index (only content entries) */
    uint64_t bytes;           /* decompressed bytes of their blobs */
} arczim_mime_stat;

typedef struct {
    uint64_t content, redirects, obsolete; /* entries by kind */
    uint64_t per_ns[128];                  /* entries per namespace byte; [0] = non-ASCII namespaces */
    arczim_mime_stat *mime;                /* arczim_mime_count() items */
    uint64_t clusters_by_comp[16];         /* clusters per compression code */
    uint64_t clusters_extended;
    uint64_t cluster_bytes;                /* decompressed bytes of all clusters (offset tables included) */
    uint64_t cluster_max_bytes;            /* largest decompressed cluster */
    uint64_t blobs;                        /* blobs in all clusters */
    uint64_t blobs_unreferenced;           /* blobs no entry points to */
    uint64_t blobs_shared;                 /* entries beyond the first that point to the same blob (aliases) */
    uint64_t html_entries;                 /* content entries in namespace C (or A) with a text/html MIME */
    uint64_t html_lru_misses[ARCZIM_LRU_SIZES];
} arczim_stats;

/*
 * arczim_check: read the whole file once and check it: every dirent (kinds, MIME, cluster and
 * redirect numbers), strictly increasing (namespace, path) order, redirect chains, every cluster
 * (decompressed once, in cluster order) and its blob offsets, and every content entry's blob number.
 * Fills *st (free with arczim_stats_free). Stops at the first error (arczim_error names the entry or
 * cluster). Does not verify the MD5 checksum: compare the file's SHA-256 with Kiwix's .sha256 instead.
 * Uses one cache slot while it runs and leaves the cache empty.
 */
bool arczim_check(arczim *z, arczim_stats *st);

/* arczim_stats_free: free what arczim_check allocated in *st. */
void arczim_stats_free(arczim_stats *st);

#endif /* ARCHOS_STACK_ARCZIM_H */
