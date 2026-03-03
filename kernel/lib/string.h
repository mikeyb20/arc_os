#ifndef ARCHOS_LIB_STRING_H
#define ARCHOS_LIB_STRING_H

#include <stddef.h>

/* strlen — return length of null-terminated string */
size_t strlen(const char *s);

/* strcmp — compare two strings, return <0, 0, or >0 */
int strcmp(const char *a, const char *b);

/* strncmp — compare at most n characters */
int strncmp(const char *a, const char *b, size_t n);

/* strncpy — copy at most n characters from src to dst, zero-pad remainder */
char *strncpy(char *dst, const char *src, size_t n);

/* strchr — find first occurrence of character c in string s */
char *strchr(const char *s, int c);

#endif /* ARCHOS_LIB_STRING_H */
