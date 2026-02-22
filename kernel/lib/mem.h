#ifndef ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_MEM_H

#include <stddef.h>

/* memcpy — copy n bytes from src to dst (no overlap) */
void *memcpy(void *dst, const void *src, size_t n);

/* memset — fill n bytes of dst with byte value c */
void *memset(void *dst, int c, size_t n);

/* memmove — copy n bytes, handles overlapping regions */
void *memmove(void *dst, const void *src, size_t n);

/* memcmp — compare n bytes, return <0, 0, or >0 */
int memcmp(const void *a, const void *b, size_t n);

#endif /* ARCHOS_LIB_MEM_H */
