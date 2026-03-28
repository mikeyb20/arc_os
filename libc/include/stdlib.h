#ifndef ARCHOS_LIBC_STDLIB_H
#define ARCHOS_LIBC_STDLIB_H

#include <stddef.h>

void    exit(int status) __attribute__((noreturn));
int     atoi(const char *s);
int     abs(int n);
void   *malloc(size_t size);
void    free(void *ptr);
void   *calloc(size_t count, size_t size);
void   *realloc(void *ptr, size_t size);

#endif /* ARCHOS_LIBC_STDLIB_H */
