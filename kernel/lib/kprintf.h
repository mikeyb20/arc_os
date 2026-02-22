#ifndef ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_KPRINTF_H

/* Kernel printf — outputs formatted text to serial (COM1).
 *
 * Supported format specifiers:
 *   %s  — string          %d  — signed int32     %u  — unsigned int32
 *   %x  — hex uint32      %p  — pointer (0x + 16 hex digits)
 *   %ld — signed int64    %lu — unsigned int64   %lx — hex uint64
 *   %%  — literal '%'
 */
void kprintf(const char *fmt, ...);

#endif /* ARCHOS_LIB_KPRINTF_H */
