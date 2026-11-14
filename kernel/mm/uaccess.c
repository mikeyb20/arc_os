/* arc_os — copies between kernel and user memory (user_access.h, M1-07)
 *
 * Owns: the range checks around arch_copy_user (arch/x86_64/uaccess.asm), the
 * single routine whose faults the exception table fixes up.
 * Invariants: every byte of user memory the kernel reads or writes goes
 * through arch_copy_user, after a check that the whole range is in the user
 * half (so a fault there is always on a user address).
 * Locks: none; the fault handler takes the mm lock for reading.
 * Cost: one bounds check and one REP MOVSB per call; strncpy_from_user copies
 * at most to the end of the page holding the NUL. */

#include "user_access.h"
#include "mm/pmm.h"
#include "arch/hal.h"
#include "lib/mem.h"
#include "uapi/errno.h"

int copy_from_user(void *dst, const void *src, size_t len) {
    if (len == 0) return 0;
    if (!user_ptr_valid(src, len)) {
        memset(dst, 0, len);
        return -EFAULT;
    }
    size_t left = hal_copy_user(dst, src, len);
    if (left != 0) {
        memset((uint8_t *)dst + (len - left), 0, left);
        return -EFAULT;
    }
    return 0;
}

size_t copy_from_user_partial(void *dst, const void *src, size_t len) {
    uint64_t start = (uint64_t)src;
    if (src == NULL || start >= USER_ADDR_LIMIT) return 0;
    if (len > USER_ADDR_LIMIT - start) len = (size_t)(USER_ADDR_LIMIT - start);   /* stop at the user half's end */
    return len - hal_copy_user(dst, src, len);
}

size_t copy_from_user_nofault(void *dst, const void *src, size_t len) {
    /* The fault path resolves nothing while this CPU's lock depth is above
     * zero (it may sleep), so the copy stops at the first missing page */
    hal_lock_depth_inc();
    size_t n = copy_from_user_partial(dst, src, len);
    hal_lock_depth_dec();
    return n;
}

int copy_to_user(void *dst, const void *src, size_t len) {
    if (len == 0) return 0;
    if (!user_ptr_valid(dst, len)) return -EFAULT;
    return hal_copy_user(dst, src, len) == 0 ? 0 : -EFAULT;
}

long strncpy_from_user(char *dst, const char *src, size_t size) {
    uint64_t addr = (uint64_t)src;
    size_t n = 0;
    while (n < size) {
        /* Up to the end of this page: if the string ends here, nothing past
         * the page holding its NUL is touched */
        uint64_t page_end = PAGE_ALIGN_DOWN(addr + n) + PAGE_SIZE;
        size_t chunk = (size_t)(page_end - (addr + n));
        if (chunk > size - n) chunk = size - n;
        if (!user_ptr_valid(src + n, chunk)) return -EFAULT;
        if (hal_copy_user(dst + n, src + n, chunk) != 0) return -EFAULT;
        for (size_t i = 0; i < chunk; i++) {
            if (dst[n + i] == '\0') return (long)(n + i);
        }
        n += chunk;
    }
    return -ENAMETOOLONG;
}
