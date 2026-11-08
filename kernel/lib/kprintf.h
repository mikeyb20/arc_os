#ifndef ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_KPRINTF_H

/* Kernel printf — outputs formatted text to serial (COM1) and any
 * registered secondary output (e.g., framebuffer console).
 *
 * Supported format specifiers:
 *   %s  — string          %d  — signed int32     %u  — unsigned int32
 *   %x  — hex uint32      %p  — pointer (0x + 16 hex digits)
 *   %ld — signed int64    %lu — unsigned int64   %lx — hex uint64
 *   %%  — literal '%'
 */
void kprintf(const char *fmt, ...);

/* Register a secondary character output function (e.g., fb_console_putchar).
 * Characters will be sent to both serial AND this callback. */
void kprintf_set_putchar_hook(void (*hook)(char c));

/* Stop permanently — use after printing a fatal error message. Halts the CPU,
 * or in kernel test mode (arc.test=) exits QEMU with a failure code at once.
 * Defined in lib/testmode.c. */
void kernel_panic_halt(void) __attribute__((noreturn));
#define KERNEL_PANIC() kernel_panic_halt()

#endif /* ARCHOS_LIB_KPRINTF_H */
