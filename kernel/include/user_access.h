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

/* Copy len bytes from user space to kernel space. Returns 0 or -EINVAL. */
static inline int copy_from_user(void *dst, const void *user_src, size_t len) {
    if (!user_ptr_valid(user_src, len)) return -22; /* -EINVAL */
    __builtin_memcpy(dst, user_src, len);
    return 0;
}

/* Copy len bytes from kernel space to user space. Returns 0 or -EINVAL. */
static inline int copy_to_user(void *user_dst, const void *src, size_t len) {
    if (!user_ptr_valid(user_dst, len)) return -22; /* -EINVAL */
    __builtin_memcpy(user_dst, src, len);
    return 0;
}

#endif /* ARCHOS_USER_ACCESS_H */
