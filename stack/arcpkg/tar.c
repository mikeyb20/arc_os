/*
 * tar.c — a .arcpkg as a stream (arcpkg.h): zstd streaming decompression with the window capped at
 * 8 MiB (ZSTD_d_windowLogMax), and a POSIX ustar reader on top of it.
 *
 * Only what a package may hold is accepted: regular files and directories, names (prefix + name) that
 * are relative paths without empty, '.' or '..' parts, sizes in octal (no base-256 extension), and a
 * correct header checksum. Data is never buffered beyond the decompressor's output block.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

#include "arcpkg.h"

#define BLOCK 512u

struct pkg_stream {
    int fd;
    ZSTD_DCtx *dctx;
    ZSTD_inBuffer in;
    unsigned char *inbuf;
    size_t in_cap;
    unsigned char *out;
    size_t out_cap;
    size_t out_len; /* bytes decompressed into out */
    size_t out_pos; /* bytes of out consumed */
    bool input_done;
    size_t last_ret;   /* ZSTD_decompressStream's last return: 0 = a frame just ended */
    uint64_t left;     /* data bytes of the current member not read yet */
    uint64_t padding;  /* padding bytes after the current member's data */
};

pkg_stream *pkg_stream_open(const char *path, char *why) {
    pkg_stream *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        pkg_fail(why, "out of memory");
        return NULL;
    }
    s->fd = open(path, O_RDONLY | O_CLOEXEC);
    s->in_cap = ZSTD_DStreamInSize();
    s->out_cap = ZSTD_DStreamOutSize();
    s->inbuf = malloc(s->in_cap);
    s->out = malloc(s->out_cap);
    s->dctx = ZSTD_createDCtx();
    if (s->fd < 0) {
        pkg_fail(why, "%s: %s", path, strerror(errno));
    } else if (s->inbuf == NULL || s->out == NULL || s->dctx == NULL) {
        pkg_fail(why, "out of memory");
    } else if (ZSTD_isError(ZSTD_DCtx_setParameter(s->dctx, ZSTD_d_windowLogMax, PKG_ZSTD_WINDOW_LOG))) {
        pkg_fail(why, "zstd: cannot limit the window");
    } else {
        s->in.src = s->inbuf;
        return s;
    }
    pkg_stream_close(s);
    return NULL;
}

void pkg_stream_close(pkg_stream *s) {
    if (s == NULL) {
        return;
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    ZSTD_freeDCtx(s->dctx);
    free(s->inbuf);
    free(s->out);
    free(s);
}

/* refill: decompress more into out (which is empty). 1 = some bytes, 0 = end of input, -1 = error. */
static int refill(pkg_stream *s, char *why) {
    s->out_len = s->out_pos = 0;
    while (s->out_len == 0) {
        if (s->in.pos == s->in.size && !s->input_done) {
            ssize_t n = read(s->fd, s->inbuf, s->in_cap);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0) {
                return pkg_fail(why, "reading the package: %s", strerror(errno)) - 1;
            }
            s->in.size = (size_t)n;
            s->in.pos = 0;
            s->input_done = n == 0;
        }
        if (s->in.pos == s->in.size && s->input_done) {
            if (s->last_ret != 0) {
                return pkg_fail(why, "the package is cut short (zstd frame not finished)") - 1;
            }
            return 0;
        }
        ZSTD_outBuffer out = {s->out, s->out_cap, 0};
        size_t ret = ZSTD_decompressStream(s->dctx, &out, &s->in);
        if (ZSTD_isError(ret)) {
            return pkg_fail(why, "the package is damaged or not zstd with a window of at most 8 MiB (%s)",
                            ZSTD_getErrorName(ret)) -
                   1;
        }
        s->last_ret = ret;
        s->out_len = out.pos;
    }
    return 1;
}

/* read_exact: n bytes of the decompressed stream into buf (NULL: skip them). 1 ok, 0 = end before any, -1 error. */
static int read_exact(pkg_stream *s, unsigned char *buf, uint64_t n, char *why) {
    uint64_t done = 0;
    while (done < n) {
        if (s->out_pos == s->out_len) {
            int r = refill(s, why);
            if (r <= 0) {
                return r < 0 ? -1 : (done == 0 ? 0 : (pkg_fail(why, "the package's tar archive is cut short"), -1));
            }
        }
        size_t take = s->out_len - s->out_pos;
        if (take > n - done) {
            take = (size_t)(n - done);
        }
        if (buf != NULL) {
            memcpy(buf + done, s->out + s->out_pos, take);
        }
        s->out_pos += take;
        done += take;
    }
    return 1;
}

/* octal: a ustar numeric field (octal digits, then NUL or space). */
static bool octal(const unsigned char *f, size_t len, uint64_t *out) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < len && f[i] == ' ') {
        i++;
    }
    size_t digits = 0;
    for (; i < len && f[i] >= '0' && f[i] <= '7'; i++, digits++) {
        if (v >> 60) {
            return false;
        }
        v = v * 8 + (uint64_t)(f[i] - '0');
    }
    for (; i < len; i++) {
        if (f[i] != ' ' && f[i] != '\0') {
            return false; /* includes the base-256 extension (high bit set) */
        }
    }
    *out = v;
    return digits > 0;
}

/* field: a NUL-terminated or full-length string field; its length. */
static size_t field(const unsigned char *f, size_t cap) {
    const unsigned char *nul = memchr(f, '\0', cap);
    return nul ? (size_t)(nul - f) : cap;
}

/* checksum_ok: the header checksum (chksum field read as spaces), as signed or unsigned bytes. */
static bool checksum_ok(const unsigned char *h) {
    uint64_t want;
    if (!octal(h + 148, 8, &want)) {
        return false;
    }
    uint64_t sum = 0;
    int64_t ssum = 0;
    for (unsigned i = 0; i < BLOCK; i++) {
        unsigned char c = (i >= 148 && i < 156) ? ' ' : h[i];
        sum += c;
        ssum += (signed char)c;
    }
    return want == sum || (int64_t)want == ssum;
}

/* member_name: prefix "/" name into m->name, trailing '/' removed, checked to be a safe relative path. */
static bool member_name(const unsigned char *h, tar_member *m, char *why) {
    size_t nlen = field(h, 100);
    size_t plen = field(h + 345, 155);
    size_t at = 0;
    if (plen > 0) {
        memcpy(m->name, h + 345, plen);
        m->name[plen] = '/';
        at = plen + 1;
    }
    memcpy(m->name + at, h, nlen);
    size_t len = at + nlen;
    while (len > 0 && m->name[len - 1] == '/') {
        len--;
    }
    m->name[len] = '\0';
    if (!arcjson_is_schema_relpath(m->name, len)) {
        return pkg_fail(why, "archive member '%s' is not a safe relative path (absolute, '..', '.' or empty part)",
                        m->name);
    }
    return true;
}

/* type_name: what a refused member type is, for the error. */
static const char *type_name(unsigned char t) {
    switch (t) {
    case '1':
        return "hard link";
    case '2':
        return "symlink";
    case '3':
    case '4':
        return "device";
    case '6':
        return "FIFO";
    case 'x':
    case 'g':
        return "pax extended header";
    case 'L':
    case 'K':
        return "GNU long name";
    default:
        return "unknown type";
    }
}

int pkg_tar_next(pkg_stream *s, tar_member *m, char *why) {
    if (read_exact(s, NULL, s->left + s->padding, why) < 0) {
        return -1;
    }
    s->left = s->padding = 0;
    unsigned char h[BLOCK];
    int r = read_exact(s, h, BLOCK, why);
    if (r <= 0) {
        return r < 0 ? -1 : (pkg_fail(why, "the package's tar archive has no end marker"), -1);
    }
    static const unsigned char zero[BLOCK];
    if (memcmp(h, zero, BLOCK) == 0) {
        return 0; /* end of archive */
    }
    if (memcmp(h + 257,
               "ustar\0"
               "00",
               8) != 0) {
        return pkg_fail(why, "not a POSIX ustar archive (magic)") - 1;
    }
    if (!checksum_ok(h)) {
        return pkg_fail(why, "tar header checksum wrong (damaged archive)") - 1;
    }
    unsigned char t = h[156];
    if (t != '0' && t != '\0' && t != '5') {
        char name[256];
        size_t n = field(h, 100);
        memcpy(name, h, n);
        name[n] = '\0';
        return pkg_fail(why, "archive member '%s' is a %s; only files and directories are allowed", name,
                        type_name(t)) -
               1;
    }
    uint64_t mode;
    if (!member_name(h, m, why)) {
        return -1;
    }
    if (!octal(h + 124, 12, &m->size) || !octal(h + 100, 8, &mode)) {
        return pkg_fail(why, "archive member '%s': size or mode is not ustar octal", m->name) - 1;
    }
    m->type = t == '5' ? TAR_DIR : TAR_FILE;
    m->mode = (unsigned)(mode & 0777);
    if (m->type == TAR_DIR) {
        m->size = 0;
    }
    s->left = m->size;
    s->padding = (BLOCK - m->size % BLOCK) % BLOCK;
    return 1;
}

long pkg_tar_read(pkg_stream *s, void *buf, size_t cap, char *why) {
    if (s->left == 0) {
        return 0;
    }
    if (s->out_pos == s->out_len) {
        int r = refill(s, why);
        if (r <= 0) {
            return r < 0 ? -1 : (pkg_fail(why, "the package's tar archive is cut short"), -1);
        }
    }
    size_t take = s->out_len - s->out_pos;
    if (take > cap) {
        take = cap;
    }
    if (take > s->left) {
        take = (size_t)s->left;
    }
    memcpy(buf, s->out + s->out_pos, take);
    s->out_pos += take;
    s->left -= take;
    return (long)take;
}
