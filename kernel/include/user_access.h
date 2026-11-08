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

/* Fault-free user copies (kernel/mm/uaccess.c). The kernel has no page-fault
 * fixups yet (M1-19), so these check that every page of the user range is
 * mapped for user access in the current address space before touching it.
 * Cost: one page-table walk per page crossed. All return -EFAULT on a bad range. */

/* Copy len bytes from user src to kernel dst. Returns 0 or -EFAULT. */
int copy_from_user(void *dst, const void *src, size_t len);

/* Copy len bytes from kernel src to user dst (pages must be writable).
 * Returns 0 or -EFAULT. */
int copy_to_user(void *dst, const void *src, size_t len);

/* Copy a NUL-terminated user string into dst[size]. Returns its length
 * (without the NUL), -EFAULT, or -ENAMETOOLONG if no NUL within size bytes. */
long strncpy_from_user(char *dst, const char *src, size_t size);

#endif /* ARCHOS_USER_ACCESS_H */
