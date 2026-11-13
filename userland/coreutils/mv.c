/* arc_os coreutil — mv: rename a file or directory (rename(2), M1-11) */

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mv <source> <dest>\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) < 0) {
        fprintf(stderr, "mv: cannot move '%s' to '%s'\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
