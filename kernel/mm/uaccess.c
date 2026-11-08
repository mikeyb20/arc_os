/* arc_os — fault-free copies between kernel and user memory (user_access.h). */

#include "user_access.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "arch/hal.h"
#include "lib/mem.h"
#include "uapi/errno.h"

/* Every page of [addr, addr+len) is mapped for user access (write if asked) */
static int user_range_ok(uint64_t addr, size_t len, int write) {
    if (!user_ptr_valid((const void *)addr, len)) return 0;
    uint64_t pml4 = hal_get_address_space() & PTE_ADDR_MASK;
    uint64_t end = addr + len;
    for (uint64_t page = PAGE_ALIGN_DOWN(addr); page < end; page += PAGE_SIZE) {
        if (!vmm_user_access_ok(pml4, page, write)) return 0;
    }
    return 1;
}

int copy_from_user(void *dst, const void *src, size_t len) {
    if (len == 0) return 0;
    if (!user_range_ok((uint64_t)src, len, 0)) return -EFAULT;
    memcpy(dst, src, len);
    return 0;
}

int copy_to_user(void *dst, const void *src, size_t len) {
    if (len == 0) return 0;
    if (!user_range_ok((uint64_t)dst, len, 1)) return -EFAULT;
    memcpy(dst, src, len);
    return 0;
}

long strncpy_from_user(char *dst, const char *src, size_t size) {
    uint64_t addr = (uint64_t)src;
    size_t n = 0;
    while (n < size) {
        /* Check each page once, then copy up to its end */
        if (!user_range_ok(addr + n, 1, 0)) return -EFAULT;
        uint64_t page_end = PAGE_ALIGN_DOWN(addr + n) + PAGE_SIZE;
        while (n < size && addr + n < page_end) {
            char c = src[n];
            dst[n] = c;
            if (c == '\0') return (long)n;
            n++;
        }
    }
    return -ENAMETOOLONG;
}
