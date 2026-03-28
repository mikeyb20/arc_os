/* arc_os coreutil — mv: move/rename file (cp + rm, no SYS_RENAME) */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mv <source> <dest>\n");
        return 1;
    }

    /* Copy source to dest */
    int src = open(argv[1], O_RDONLY);
    if (src < 0) {
        fprintf(stderr, "mv: cannot open '%s'\n", argv[1]);
        return 1;
    }

    int dst = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (dst < 0) {
        fprintf(stderr, "mv: cannot create '%s'\n", argv[2]);
        close(src);
        return 1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0)
        write(dst, buf, (size_t)n);

    close(src);
    close(dst);

    /* Remove source */
    if (unlink(argv[1]) < 0) {
        fprintf(stderr, "mv: cannot remove source '%s'\n", argv[1]);
        return 1;
    }
    return 0;
}
