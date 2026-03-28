/* arc_os coreutil — chmod: change file mode */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Parse octal mode string */
static mode_t parse_mode(const char *s) {
    mode_t m = 0;
    while (*s >= '0' && *s <= '7') {
        m = (m << 3) | (mode_t)(*s - '0');
        s++;
    }
    return m;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: chmod <mode> <file>...\n");
        return 1;
    }
    mode_t mode = parse_mode(argv[1]);
    int ret = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], mode) < 0) {
            fprintf(stderr, "chmod: cannot change mode of '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
