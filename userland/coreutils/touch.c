/* arc_os coreutil — touch: create empty file */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: touch <file>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT | O_WRONLY);
        if (fd < 0) {
            fprintf(stderr, "touch: cannot create '%s'\n", argv[i]);
            continue;
        }
        close(fd);
    }
    return 0;
}
