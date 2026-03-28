/* arc_os coreutil — grep: fixed-string search (no regex) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int grep_fd(int fd, const char *pattern, const char *filename) {
    char buf[4096];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0)
        total += n;
    buf[total] = '\0';

    int found = 0;
    char *line = buf;
    while (line < buf + total) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strstr(line, pattern)) {
            if (filename)
                printf("%s:%s\n", filename, line);
            else
                printf("%s\n", line);
            found = 1;
        }

        if (nl) { line = nl + 1; }
        else break;
    }
    return found;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: grep <pattern> [file]...\n");
        return 2;
    }

    const char *pattern = argv[1];
    int found = 0;

    if (argc == 2) {
        /* Read from stdin */
        found = grep_fd(0, pattern, NULL);
    } else {
        int multi = (argc > 3);
        for (int i = 2; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "grep: %s: No such file\n", argv[i]);
                continue;
            }
            if (grep_fd(fd, pattern, multi ? argv[i] : NULL))
                found = 1;
            close(fd);
        }
    }
    return found ? 0 : 1;
}
