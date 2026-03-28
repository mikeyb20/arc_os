#include "fs/passwd.h"
#include "lib/string.h"
#include "lib/mem.h"

/* Copy up to max-1 chars from src to dst, stopping at delimiter or end.
 * Returns pointer to character after delimiter, or NULL if end of string. */
static const char *copy_field(const char *src, const char *end,
                               char *dst, int max, char delim) {
    int i = 0;
    while (src < end && *src != delim && *src != '\n' && *src != '\0') {
        if (i < max - 1) {
            dst[i++] = *src;
        }
        src++;
    }
    dst[i] = '\0';

    if (src < end && *src == delim) {
        return src + 1;  /* Skip delimiter */
    }
    return src;
}

/* Parse an unsigned integer from a colon-delimited field. */
static const char *parse_uint(const char *src, const char *end,
                                uint32_t *out, char delim) {
    *out = 0;
    while (src < end && *src != delim && *src != '\n' && *src != '\0') {
        if (*src >= '0' && *src <= '9') {
            *out = *out * 10 + (*src - '0');
        }
        src++;
    }
    if (src < end && *src == delim) {
        return src + 1;
    }
    return src;
}

int passwd_parse(const char *buf, uint32_t buf_len,
                 PasswdEntry *entries, int max_entries) {
    if (buf == NULL || entries == NULL || max_entries <= 0) return -1;

    const char *ptr = buf;
    const char *end = buf + buf_len;
    int count = 0;

    while (ptr < end && count < max_entries) {
        /* Skip blank lines */
        while (ptr < end && (*ptr == '\n' || *ptr == '\r')) ptr++;
        if (ptr >= end) break;

        /* Skip comment lines */
        if (*ptr == '#') {
            while (ptr < end && *ptr != '\n') ptr++;
            continue;
        }

        PasswdEntry *e = &entries[count];
        memset(e, 0, sizeof(*e));

        /* Format: name:password:uid:gid:gecos:home:shell */
        ptr = copy_field(ptr, end, e->name, PASSWD_NAME_MAX, ':');
        ptr = copy_field(ptr, end, e->password, PASSWD_PASS_MAX, ':');
        ptr = parse_uint(ptr, end, &e->uid, ':');
        ptr = parse_uint(ptr, end, &e->gid, ':');

        /* Skip gecos field */
        char gecos[64];
        ptr = copy_field(ptr, end, gecos, sizeof(gecos), ':');

        ptr = copy_field(ptr, end, e->home, PASSWD_HOME_MAX, ':');
        ptr = copy_field(ptr, end, e->shell, PASSWD_SHELL_MAX, ':');

        /* Skip to next line */
        while (ptr < end && *ptr != '\n') ptr++;

        if (e->name[0] != '\0') {
            count++;
        }
    }

    return count;
}

const PasswdEntry *passwd_find_user(const PasswdEntry *entries, int count,
                                     const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

const PasswdEntry *passwd_find_uid(const PasswdEntry *entries, int count,
                                    uint32_t uid) {
    for (int i = 0; i < count; i++) {
        if (entries[i].uid == uid) {
            return &entries[i];
        }
    }
    return NULL;
}
