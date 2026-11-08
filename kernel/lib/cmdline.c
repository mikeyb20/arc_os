#include "lib/cmdline.h"

static char g_cmdline[CMDLINE_MAX];

void cmdline_set(const char *line) {
    size_t i = 0;
    if (line != NULL) {
        for (; line[i] != '\0' && i < CMDLINE_MAX - 1; i++) {
            g_cmdline[i] = line[i];
        }
    }
    g_cmdline[i] = '\0';
}

const char *cmdline_raw(void) {
    return g_cmdline;
}

int cmdline_get(const char *line, const char *key, char *out, size_t outsz) {
    if (line == NULL || key == NULL || key[0] == '\0') return 0;

    const char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        /* Compare this token's name against key */
        const char *k = key;
        const char *t = p;
        while (*k != '\0' && *t == *k) { k++; t++; }

        if (*k == '\0' && (*t == '=' || *t == ' ' || *t == '\t' || *t == '\0')) {
            const char *v = (*t == '=') ? t + 1 : t;
            if (out != NULL && outsz > 0) {
                size_t n = 0;
                if (*t == '=') {
                    while (v[n] != '\0' && v[n] != ' ' && v[n] != '\t' && n < outsz - 1) {
                        out[n] = v[n];
                        n++;
                    }
                }
                out[n] = '\0';
            }
            return 1;
        }

        /* Skip to the next token */
        while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    }
    return 0;
}
