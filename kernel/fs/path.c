#include "fs/path.h"
#include "lib/string.h"

/* Error codes (match vfs.h) */
#define EINVAL       22
#define ENAMETOOLONG 36

/* Append a component to the output path. Returns 0 on success, -ENAMETOOLONG if overflow. */
static int append_component(char *out, size_t *pos, size_t out_size,
                            const char *comp, size_t comp_len) {
    /* Need room for '/' + component + NUL */
    if (*pos + 1 + comp_len >= out_size) return -ENAMETOOLONG;
    out[*pos] = '/';
    (*pos)++;
    for (size_t i = 0; i < comp_len; i++) {
        out[*pos] = comp[i];
        (*pos)++;
    }
    out[*pos] = '\0';
    return 0;
}

/* Pop the last component from the output path (go up one directory). */
static void pop_component(char *out, size_t *pos) {
    /* Scan backward to find the previous '/' */
    while (*pos > 0 && out[*pos - 1] != '/') {
        (*pos)--;
    }
    /* Remove the trailing '/' unless we're at root */
    if (*pos > 1) {
        (*pos)--;
    }
    out[*pos] = '\0';
}

/* Process a single input string component-by-component into out. */
static int process_input(const char *src, char *out, size_t *pos, size_t out_size) {
    size_t i = 0;
    size_t len = strlen(src);

    while (i < len) {
        /* Skip slashes */
        while (i < len && src[i] == '/') i++;
        if (i >= len) break;

        /* Find end of component */
        size_t start = i;
        while (i < len && src[i] != '/') i++;
        size_t comp_len = i - start;

        /* "." — skip */
        if (comp_len == 1 && src[start] == '.') {
            continue;
        }

        /* ".." — pop */
        if (comp_len == 2 && src[start] == '.' && src[start + 1] == '.') {
            pop_component(out, pos);
            continue;
        }

        /* Normal component — append */
        int err = append_component(out, pos, out_size, &src[start], comp_len);
        if (err != 0) return err;
    }

    return 0;
}

int path_normalize(const char *cwd, const char *input, char *out, size_t out_size) {
    if (input == NULL || out == NULL || out_size < 2) return -EINVAL;

    /* Start with empty output */
    out[0] = '\0';
    size_t pos = 0;

    if (input[0] == '/') {
        /* Absolute path — process input directly */
        int err = process_input(input, out, &pos, out_size);
        if (err != 0) return err;
    } else {
        /* Relative path — first process cwd, then input */
        if (cwd == NULL || cwd[0] != '/') return -EINVAL;

        int err = process_input(cwd, out, &pos, out_size);
        if (err != 0) return err;

        err = process_input(input, out, &pos, out_size);
        if (err != 0) return err;
    }

    /* Empty result means root */
    if (pos == 0) {
        if (out_size < 2) return -ENAMETOOLONG;
        out[0] = '/';
        out[1] = '\0';
    }

    return 0;
}
