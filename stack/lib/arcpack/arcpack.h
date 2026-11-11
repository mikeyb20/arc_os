/*
 * arcpack — the knowledge-pack reader and writer (M3-02, ADR-0011). Format: docs/formats/pack-v1.md.
 *
 * Reader: arcpack_open() maps content/docs.idx, content/strings, content/docs.zst and chunks/chunks.idx
 * read-only, checks their headers against each other and manifest.json, and nothing else; every offset
 * and length taken from a record is checked when it is used, so a damaged pack gives an error, never a
 * crash. Document text is decompressed into the caller's buffer.
 *
 * Writer: streams documents into the files as they are added (memory does not grow with the pack),
 * checks the text markup and chunk ranges, and replaces the files only when arcpack_writer_finish()
 * succeeds. The manifest is the pipeline's job (M3-04).
 *
 * This is the only code that calls zstd on pack content. Plain frames only: no dictionary, window
 * at most 8 MiB, content checksum on; the writer makes only these and the reader refuses others.
 *
 * Cost: document and chunk lookup and chunk-ID parsing are O(1); opening is O(1) in the pack's size
 * (plus parsing manifest.json). Reading a chunk decompresses its one document. 32 bytes per document
 * and per chunk on disk.
 */
#ifndef ARCHOS_STACK_ARCPACK_H
#define ARCHOS_STACK_ARCPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARCPACK_SCHEMA_VERSION  1
#define ARCPACK_HEADER_SIZE     4096u
#define ARCPACK_NAME_MAX        64u /* bytes of the pack name and of the version in a header */
#define ARCPACK_MAX_TAGS        64u
/* Longest chunk ID with its NUL: name ":" u32 ":" u32. */
#define ARCPACK_CHUNK_ID_MAX    (ARCPACK_NAME_MAX + 1 + 10 + 1 + 10 + 1)
/* The writer's zstd frames: window at most 2^23 = 8 MiB, compression level 19 (no --ultra). */
#define ARCPACK_ZSTD_WINDOW_LOG 23
#define ARCPACK_ZSTD_LEVEL      19

/* Markup bytes in document text (pack-v1.md, "Document text"). */
#define ARCPACK_MARK_HEADING  '\x01'
#define ARCPACK_MARK_LINK     '\x02'
#define ARCPACK_MARK_LINK_END '\x03'
#define ARCPACK_MARK_IMAGE    '\x04'
#define ARCPACK_MARK_SEP      '\x1F' /* link target | text, image reference | caption, section path parts */

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "pack files are little-endian, read in place");

/* The first 152 bytes of every file's 4096-byte header; the rest is zero. */
typedef struct {
    char magic[8];
    uint32_t schema_version;
    uint32_t record_size;
    uint64_t count;
    char pack_name[ARCPACK_NAME_MAX];    /* NUL-padded */
    char pack_version[ARCPACK_NAME_MAX]; /* NUL-padded */
} arcpack_file_header;
_Static_assert(sizeof(arcpack_file_header) == 152, "pack-v1.md header layout");

/* content/docs.idx record. */
typedef struct {
    uint64_t frame_offset;
    uint32_t frame_size;
    uint32_t text_size;
    uint32_t title_offset;
    uint32_t title_length;
    uint32_t first_chunk;
    uint32_t chunk_count;
} arcpack_doc_record;
_Static_assert(sizeof(arcpack_doc_record) == 32, "pack-v1.md document record");

/* chunks/chunks.idx record. */
typedef struct {
    uint32_t document;
    uint32_t offset;
    uint32_t length;
    uint32_t section_offset;
    uint32_t section_length;
    uint32_t reserved; /* zero */
    uint64_t tags;
} arcpack_chunk_record;
_Static_assert(sizeof(arcpack_chunk_record) == 32, "pack-v1.md chunk record");

/* ---- Reader --------------------------------------------------------------------------------- */

typedef struct arcpack arcpack;

/* A document, checked against the files. `title` points into the mapped strings (not NUL-terminated). */
typedef struct {
    const char *title;
    size_t title_length;
    uint32_t text_size; /* buffer size arcpack_doc_text() needs */
    uint32_t first_chunk;
    uint32_t chunk_count;
} arcpack_doc;

/* A chunk, checked against its document. `section` points into the mapped strings (not NUL-terminated). */
typedef struct {
    uint32_t document;
    uint32_t number; /* chunk number inside the document */
    uint32_t offset; /* in the document's text */
    uint32_t length;
    uint32_t doc_text_size; /* buffer size arcpack_chunk_text() needs */
    const char *section;    /* parts separated by ARCPACK_MARK_SEP */
    size_t section_length;
    uint64_t tags;
} arcpack_chunk;

/*
 * arcpack_open: open the pack in directory `dir`. Returns NULL with *why set (a static or per-call
 * message, valid until the next arcpack_open on this thread) if a file is missing, a header or
 * the manifest is wrong, or the files disagree.
 */
arcpack *arcpack_open(const char *dir, const char **why);

/* arcpack_close: unmap and free; NULL is fine. */
void arcpack_close(arcpack *p);

/* arcpack_error: why the last failed call on `p` failed. */
const char *arcpack_error(const arcpack *p);

/* The manifest's name and version (NUL-terminated), and its domain tags in bit order. */
const char *arcpack_name(const arcpack *p);
const char *arcpack_version(const arcpack *p);
size_t arcpack_tag_count(const arcpack *p);
const char *arcpack_tag_name(const arcpack *p, size_t bit); /* NULL if bit >= arcpack_tag_count() */

/* Number of documents and chunks. */
uint32_t arcpack_doc_count(const arcpack *p);
uint32_t arcpack_chunk_count(const arcpack *p);

/* arcpack_doc_get: document `doc`'s record, checked; false (see arcpack_error) if out of range or damaged. */
bool arcpack_doc_get(arcpack *p, uint32_t doc, arcpack_doc *out);

/*
 * arcpack_doc_text: decompress document `doc`'s marked-up text into buf (cap >= its text_size;
 * not NUL-terminated); *len = text_size. False if the buffer is too small or the frame is damaged.
 */
bool arcpack_doc_text(arcpack *p, uint32_t doc, char *buf, size_t cap, size_t *len);

/* arcpack_chunk_get: chunk `index`'s record, checked against its document. */
bool arcpack_chunk_get(arcpack *p, uint32_t index, arcpack_chunk *out);

/*
 * arcpack_chunk_text: the marked-up text of chunk `index`, at the start of buf (not NUL-terminated).
 * It decompresses the whole document into buf, so cap must be >= the chunk's doc_text_size.
 */
bool arcpack_chunk_text(arcpack *p, uint32_t index, char *buf, size_t cap, size_t *len);

/* arcpack_chunk_id: write chunk `index`'s ID ("pack:doc:n", NUL-terminated) into buf; cap >= ARCPACK_CHUNK_ID_MAX. */
bool arcpack_chunk_id(arcpack *p, uint32_t index, char *buf, size_t cap);

/* arcpack_chunk_parse: the global chunk index of chunk ID id[0..len); false if malformed, another pack's, or out of range. */
bool arcpack_chunk_parse(arcpack *p, const char *id, size_t len, uint32_t *index);

/*
 * arcpack_plain: the plain text of marked-up text src[0..len) into dst (dst may equal src); returns
 * its length, which is <= len. Heading markers go, links become their text, image lines their caption.
 */
size_t arcpack_plain(char *dst, const char *src, size_t len);

/* ---- Writer --------------------------------------------------------------------------------- */

typedef struct arcpack_writer arcpack_writer;

/* One chunk of a document being added. */
typedef struct {
    uint32_t offset; /* byte range in the document's text */
    uint32_t length;
    const char *section; /* section path, parts separated by ARCPACK_MARK_SEP; NULL or "" for none */
    size_t section_length;
    uint64_t tags;
} arcpack_chunk_spec;

/*
 * arcpack_writer_open: start writing pack `name` (a manifest id: a-z, 0-9, '-') version `version`
 * (semver characters, at most 64 bytes) into `dir`, creating dir, dir/content and dir/chunks.
 * Files are written under temporary names; NULL with *why on failure.
 */
arcpack_writer *arcpack_writer_open(const char *dir, const char *name, const char *version, const char **why);

/*
 * arcpack_writer_add_doc: add the next document (its ID is the number added before it). Title,
 * text and section paths must follow pack-v1.md and every chunk range must be inside the text and
 * cut on character and markup boundaries. A document that fails these checks is not added
 * (*why says why) and the writer can go on; an I/O error fails the whole writer.
 */
bool arcpack_writer_add_doc(arcpack_writer *w, const char *title, size_t title_len, const char *text, size_t text_len,
                            const arcpack_chunk_spec *chunks, size_t n_chunks, const char **why);

/* Documents and chunks added so far. */
uint32_t arcpack_writer_doc_count(const arcpack_writer *w);
uint32_t arcpack_writer_chunk_count(const arcpack_writer *w);

/*
 * arcpack_writer_finish: check the links, write the headers, fsync and rename the files into place.
 * Frees the writer whatever the outcome; false with *why if the pack was not written.
 */
bool arcpack_writer_finish(arcpack_writer *w, const char **why);

/* arcpack_writer_abort: discard everything written and free the writer; NULL is fine. */
void arcpack_writer_abort(arcpack_writer *w);

#endif /* ARCHOS_STACK_ARCPACK_H */
