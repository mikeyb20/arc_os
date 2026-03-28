/* arc_os coreutil — cat: concatenate and print files */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        /* Read from stdin */
        char buf[512];
        ssize_t n;
        while ((n = read(0, buf, sizeof(buf))) > 0)
            write(1, buf, (size_t)n);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: No such file\n", argv[i]);
            continue;
        }
        char buf[512];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (size_t)n);
        close(fd);
    }
    return 0;
}
