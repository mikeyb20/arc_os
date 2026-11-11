#ifndef ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

/* Kernel printf. Every message goes into the kernel log ring (lib/klog.h),
 * which the serial and screen consoles drain; kprintf never waits for another
 * CPU's console output (M1-04).
 *
 * Format: %[-][0][width][.precision][l|ll]conv
 *   %s  string (precision = at most that many bytes)   %c  character
 *   %d  signed int32     %u  unsigned int32     %x / %X  hex uint32
 *   %ld signed int64     %lu unsigned int64     %lx / %lX hex uint64
 *   %p  pointer (0x + 16 hex digits)            %%  literal '%'
 * Width pads with spaces (or zeros with '0'); '-' pads on the right.
 */

/* Log levels, as Linux numbers them. A console prints a line only when its
 * level is at or below the console level (arc.loglevel=N, default KLOG_INFO). */
#define KLOG_EMERG 0
#define KLOG_ERR   3
#define KLOG_WARN  4
#define KLOG_INFO  6
#define KLOG_DEBUG 7

/* Log at KLOG_INFO. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Log at the given KLOG_* level. Hot paths log at KLOG_DEBUG only (ADR-0019). */
void kprintf_level(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Debug-level message: stored in the log, shown only with arc.loglevel=7. */
#define kdebug(...) kprintf_level(KLOG_DEBUG, __VA_ARGS__)

/* Format into buf (always NUL-terminated when size > 0). Returns the length
 * the full output would have had, like C99 snprintf. */
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ksnprintf with a va_list. */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) __attribute__((format(printf, 3, 0)));

/* Register the screen console: it receives the log's text (level markers
 * removed, lines above the console level skipped) after serial. */
void kprintf_set_putchar_hook(void (*hook)(char c));

/* Stop permanently — use after printing a fatal error message. Shows the panic
 * screen, then halts, or in kernel test mode (arc.test=) exits QEMU with a
 * failure code at once. Defined in lib/panic.c. */
void kernel_panic_halt(void) __attribute__((noreturn));
#define KERNEL_PANIC() kernel_panic_halt()

#endif /* ARCHOS_LIB_KPRINTF_H */
