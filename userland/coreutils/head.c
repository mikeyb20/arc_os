/* arc_os coreutil — head: print first N lines */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    int nlines = 10;
    int argstart = 1;

    if (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'n') {
        nlines = atoi(argv[2]);
        argstart = 3;
    }

    int fd = 0; /* stdin */
    if (argstart < argc) {
        fd = open(argv[argstart], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "head: %s: No such file\n", argv[argstart]);
            return 1;
        }
    }

    int count = 0;
    char c;
    while (count < nlines && read(fd, &c, 1) == 1) {
        write(1, &c, 1);
        if (c == '\n') count++;
    }
    if (fd > 0) close(fd);
    return 0;
}
