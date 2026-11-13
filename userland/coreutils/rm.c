/* arc_os coreutil — rm: remove files; -d removes empty directories too
 * (since M1-11 unlink() refuses directories, as on Linux) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv) {
    int first = 1, dirs = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        dirs = 1;
        first = 2;
    }
    if (argc <= first) {
        fprintf(stderr, "usage: rm [-d] <file>...\n");
        return 1;
    }
    int ret = 0;
    for (int i = first; i < argc; i++) {
        int r = unlink(argv[i]);
        if (r < 0 && dirs && errno == EISDIR) r = rmdir(argv[i]);
        if (r < 0) {
            fprintf(stderr, "rm: cannot remove '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
