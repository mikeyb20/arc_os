#ifndef ARCHOS_USER_ACCESS_H
#define ARCHOS_USER_ACCESS_H

#include <stdint.h>
#include <stddef.h>

/* Maximum user-space virtual address (canonical lower half boundary) */
#define USER_ADDR_LIMIT 0x0000800000000000ULL

/* Check if a user-space buffer [ptr, ptr+len) is entirely in user space. */
static inline int user_ptr_valid(const void *ptr, size_t len) {
    if (ptr == NULL || len == 0) return 0;
    uint64_t start = (uint64_t)ptr;
    uint64_t end = start + len;
    /* Check: no overflow, entirely below limit */
    return (end > start) && (end <= USER_ADDR_LIMIT);
}

#endif /* ARCHOS_USER_ACCESS_H */
