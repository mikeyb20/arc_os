/* arc_os coreutil — wc: word, line, byte count */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

static void count(int fd, const char *name) {
    int lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[512];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    if (name)
        printf("  %d  %d  %d %s\n", lines, words, bytes, name);
    else
        printf("  %d  %d  %d\n", lines, words, bytes);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        count(0, NULL);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: No such file\n", argv[i]);
            continue;
        }
        count(fd, argv[i]);
        close(fd);
    }
    return 0;
}
