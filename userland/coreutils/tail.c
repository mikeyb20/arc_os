/* arc_os coreutil — tail: print last N lines */

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

    int fd = 0;
    if (argstart < argc) {
        fd = open(argv[argstart], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "tail: %s: No such file\n", argv[argstart]);
            return 1;
        }
    }

    /* Read entire file into buffer (simple approach) */
    char buf[8192];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, sizeof(buf) - (size_t)total)) > 0)
        total += n;
    if (fd > 0) close(fd);

    /* Find start of last N lines */
    int count = 0;
    ssize_t pos = total;
    while (pos > 0 && count < nlines) {
        pos--;
        if (buf[pos] == '\n') count++;
    }
    if (pos > 0) pos++; /* Skip past the '\n' we stopped on */

    write(1, buf + pos, (size_t)(total - pos));
    return 0;
}
