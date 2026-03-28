#ifndef ARCHOS_LIBC_STDIO_H
#define ARCHOS_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* FILE stream */
typedef struct {
    int   fd;
    int   eof;
    int   error;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

/* Formatted output */
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* String output */
int puts(const char *s);
int fputs(const char *s, FILE *stream);
int fputc(int c, FILE *stream);
int putchar(int c);

/* Character input */
int fgetc(FILE *stream);
int getchar(void);
char *fgets(char *buf, int size, FILE *stream);

/* File stream operations */
FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream);

#endif /* ARCHOS_LIBC_STDIO_H */
