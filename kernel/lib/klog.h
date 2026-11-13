#ifndef ARCHOS_LIB_KLOG_H
#define ARCHOS_LIB_KLOG_H

/* Kernel log ring (M1-04).
 *
 * One byte ring holds the kernel log as Linux /proc/kmsg text: every line
 * starts with "<L>", L the level digit (KLOG_* in lib/kprintf.h). Positions
 * are byte counts since boot; the ring keeps the newest bytes,
 * [klog_tail(), klog_head()). It starts in a 16 KiB static buffer and moves
 * to a 256 KiB one once the page allocator is up (klog_set_buffer).
 *
 * Writers append under an IRQ-safe leaf lock (work-blocks §7 level 10) and
 * then try to drain the consoles: whoever takes the console prints everything
 * that is new, others return at once (they never wait for a console). After a
 * panic starts, both locks are skipped so the panic report always gets out.
 * kprintf is the usual writer; call these directly only for log plumbing. */

#include <stdint.h>
#include <stddef.h>

/* Size of the static ring used until klog_set_buffer. */
#define KLOG_EARLY_SIZE (16u * 1024)

/* Size of the ring kmain hands over once the page allocator works. */
#define KLOG_SIZE  (256u * 1024)
#define KLOG_ORDER 6   /* KLOG_SIZE = 4 KiB << KLOG_ORDER, one buddy block */

/* Append n bytes at level (0-7), then drain the consoles if no one else is. */
void klog_write(int level, const char *s, size_t n);

/* Move the log into buf (size a power of two, at least KLOG_EARLY_SIZE),
 * keeping the newest bytes. Called once, after the page allocator is up. */
void klog_set_buffer(char *buf, size_t size);

/* Lines above this level are stored but not printed on the consoles. */
void klog_set_console_level(int level);

/* The console level (KLOG_* number). */
int klog_console_level(void);

/* Print everything not yet printed, unless another context owns the consoles. */
void klog_flush(void);

/* Take the consoles for a screen redraw (scrollback). Returns 1 if taken,
 * 0 if busy. klog_console_release drains anything logged meanwhile. */
int klog_console_try_acquire(void);
void klog_console_release(void);

/* Take the consoles to draw on the screen outside the log (TTY output, VT
 * switch, M1-05): waits at most 5 ms (TSC time) for another CPU's owner to
 * finish its drain. Returns 1 if taken (release with klog_console_release),
 * 0 if the owner kept them past the bound or is this CPU (the code this
 * interrupted); the caller then draws anyway (a torn line beats lost output). */
int klog_console_acquire(void);

/* Panic: from now on writers skip the log lock (after a bounded try) and
 * the console lock, and klog_write prints at once. Never turned off. */
void klog_panic_mode(void);

/* Position of the oldest byte still in the ring, and one past the newest. */
uint64_t klog_tail(void);
uint64_t klog_head(void);

/* Copy up to n log bytes from *pos into out and advance *pos. If *pos is
 * older than the tail (overwritten), reading starts at the tail. Returns the
 * number of bytes copied (0 at the head). */
size_t klog_read(uint64_t *pos, char *out, size_t n);

/* Longest line klog_for_each_line hands over (the rest of a longer one is cut). */
#define KLOG_LINE_MAX 256

/* Call fn for each whole log line from position `from` on (skipping the line
 * `from` falls inside, unless it is 0) whose level is at or below max_level,
 * without its "<L>" marker or newline, cut at KLOG_LINE_MAX bytes. Lines
 * written meanwhile are included; bytes overwritten meanwhile are skipped
 * (klog_read). Takes no lock beyond klog_read's. Cost: O(bytes from `from` to
 * the head); for diagnostics, scrollback and panic paths only. */
void klog_for_each_line(uint64_t from, int max_level, void (*fn)(const char *line, size_t len, void *ctx), void *ctx);

#endif /* ARCHOS_LIB_KLOG_H */
