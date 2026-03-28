/* arc_os libc — FILE stream operations */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Static FILE objects for standard streams */
static FILE _stdin  = { .fd = 0 };
static FILE _stdout = { .fd = 1 };
static FILE _stderr = { .fd = 2 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

int puts(const char *s) {
    size_t len = strlen(s);
    write(1, s, len);
    write(1, "\n", 1);
    return (int)len + 1;
}

int fputs(const char *s, FILE *stream) {
    size_t len = strlen(s);
    return (int)write(stream->fd, s, len);
}

int fputc(int c, FILE *stream) {
    char ch = (char)c;
    ssize_t n = write(stream->fd, &ch, 1);
    return (n == 1) ? (unsigned char)ch : EOF;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fgetc(FILE *stream) {
    char ch;
    ssize_t n = read(stream->fd, &ch, 1);
    if (n <= 0) { stream->eof = 1; return EOF; }
    return (unsigned char)ch;
}

int getchar(void) {
    return fgetc(stdin);
}

char *fgets(char *buf, int size, FILE *stream) {
    if (size <= 0) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) { if (i == 0) return NULL; break; }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = O_RDONLY;
    if (strcmp(mode, "r") == 0) flags = O_RDONLY;
    else if (strcmp(mode, "w") == 0) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strcmp(mode, "a") == 0) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (strcmp(mode, "r+") == 0) flags = O_RDWR;
    else if (strcmp(mode, "w+") == 0) flags = O_RDWR | O_CREAT | O_TRUNC;

    int fd = open(path, flags);
    if (fd < 0) return NULL;

    FILE *f = malloc(sizeof(FILE));
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    f->eof = 0;
    f->error = 0;
    return f;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;
    int ret = close(stream->fd);
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return ret;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream) {
    size_t total = size * count;
    if (total == 0) return 0;
    ssize_t n = read(stream->fd, ptr, total);
    if (n <= 0) { stream->eof = 1; return 0; }
    return (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
    size_t total = size * count;
    if (total == 0) return 0;
    ssize_t n = write(stream->fd, ptr, total);
    if (n < 0) { stream->error = 1; return 0; }
    return (size_t)n / size;
}
