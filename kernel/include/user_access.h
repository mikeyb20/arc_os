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

/* The only way kernel code touches user memory (work-blocks.md §7; SMAP
 * enforces it from M1-19). Implemented in kernel/mm/uaccess.c on top of one
 * copy routine with an exception-table fixup (M1-07): a page that isn't there
 * yet is faulted in (demand paging, stack growth); an address the process
 * can't use ends the copy with -EFAULT instead of a kernel crash.
 *
 * Rules: never call these while holding a spinlock (the fault can't be
 * resolved then and the copy fails with -EFAULT) or the current process's mm
 * lock (the fault takes it). Cost: one REP MOVSB, plus a page fault per page
 * not yet mapped. */

/* Copy len bytes from user src to kernel dst. Returns 0 or -EFAULT (then the
 * bytes not copied are zeroed in dst). */
int copy_from_user(void *dst, const void *src, size_t len);

/* Copy up to len bytes from user src to kernel dst, stopping at the first
 * byte that faults. Returns the bytes copied; dst past them is untouched (a
 * write into file data must not change bytes it didn't take). */
size_t copy_from_user_partial(void *dst, const void *src, size_t len);

/* As copy_from_user_partial, but a page that isn't mapped yet ends the copy
 * instead of being faulted in: for copies while holding a page lock, which a
 * fault could need (mm/filemap.h). Callers fault the range in first and retry
 * on a short copy. Never sleeps. */
size_t copy_from_user_nofault(void *dst, const void *src, size_t len);

/* Copy len bytes from kernel src to user dst. Returns 0 or -EFAULT (some
 * bytes may have been written). */
int copy_to_user(void *dst, const void *src, size_t len);

/* Copy a NUL-terminated user string into dst[size]. Returns its length
 * (without the NUL), -EFAULT, or -ENAMETOOLONG if no NUL within size bytes. */
long strncpy_from_user(char *dst, const char *src, size_t size);

/* One value of x's type from / to a user pointer. Return 0 or -EFAULT. */
#define get_user(x, uptr) copy_from_user(&(x), (const void *)(uptr), sizeof(x))
#define put_user(x, uptr)                                                    \
    ({                                                                       \
        __typeof__(x) put_user_val_ = (x);                                   \
        copy_to_user((void *)(uptr), &put_user_val_, sizeof(put_user_val_)); \
    })

#endif /* ARCHOS_USER_ACCESS_H */
