/* arc_os — Kernel log ring and console drain (M1-04)
 *
 * Owns: the kernel log (one byte ring in /proc/kmsg text format, see klog.h)
 * and the drain that copies new log bytes to the serial port and the screen.
 * Invariants: the ring size is a power of two; head only grows; the byte at
 * position p is ring[p & (size - 1)] for p in [head - size, head); every line
 * in the ring starts with a 3-byte "<L>" marker; only the console owner moves
 * cons_pos and the console parser state.
 * Locks: log_lock (IRQ-safe leaf spinlock, work-blocks §7 level 10) guards the
 * ring bytes, head, line_start and the buffer switch; it is held only for a
 * copy of at most a line, never while printing. console_owner is a try-only
 * flag: a writer that finds it taken leaves its bytes for the owner, which
 * re-checks for new bytes after letting go. In panic mode both are skipped.
 * Cost: a write is O(message length) under the lock; printing is paid by the
 * console owner, outside the log lock. */

#include "lib/klog.h"
#include "lib/kprintf.h"
#include "proc/spinlock.h"
#include "arch/hal.h"

/* Bounded tries for the log lock in panic mode: the owner may be the CPU that
 * faulted inside klog_write, so the panic report must not wait for it. */
#define KLOG_PANIC_LOCK_TRIES 1000000

static char early_ring[KLOG_EARLY_SIZE];
static char *ring = early_ring;
static uint64_t ring_mask = KLOG_EARLY_SIZE - 1;
static uint64_t head;           /* bytes written since boot (atomic loads outside the lock) */
static int line_start = 1;      /* next byte begins a line: write a marker first */
static Spinlock log_lock = SPINLOCK_INIT;

static int console_owner;       /* 1 while some context prints to the consoles */
static uint64_t cons_pos;       /* next log byte the consoles have not seen */
static int console_level = KLOG_INFO;
static int panicking;
static int lock_busted;         /* panic mode gave up on log_lock once: don't wait again */

static void (*screen_hook)(char c);

/* Console parser: strips "<L>" markers, skips lines above console_level. */
static int cons_marker = 3;     /* bytes of the current marker still to read (0-3) */
static int cons_skip;           /* current line is above the console level */
static int cons_resync;         /* lost bytes: skip to the next line start */

void kprintf_set_putchar_hook(void (*hook)(char c)) {
    screen_hook = hook;
}

void klog_set_console_level(int level) {
    console_level = level < 0 ? 0 : (level > KLOG_DEBUG ? KLOG_DEBUG : level);
}

int klog_console_level(void) {
    return console_level;
}

void klog_panic_mode(void) {
    __atomic_store_n(&panicking, 1, __ATOMIC_SEQ_CST);
}

/* How log_lock_take got the lock: pass the result to log_unlock */
#define LOCK_NONE 0   /* panic mode, lock busted: no lock held */
#define LOCK_IRQ  1   /* spinlock_acquire: interrupts off, flags saved */
#define LOCK_RAW  2   /* panic mode: bare test-and-set */

/* Take log_lock; in panic mode give up after a bounded number of tries, once. */
static int log_lock_take(void) {
    if (!__atomic_load_n(&panicking, __ATOMIC_RELAXED)) {
        spinlock_acquire(&log_lock);
        return LOCK_IRQ;
    }
    if (lock_busted) return LOCK_NONE;
    for (int i = 0; i < KLOG_PANIC_LOCK_TRIES; i++) {
        if (!__sync_lock_test_and_set(&log_lock.locked, 1)) return LOCK_RAW;
        hal_cpu_relax();
    }
    lock_busted = 1;   /* its holder is stuck (likely this CPU): write without it */
    return LOCK_NONE;
}

static void log_unlock(int how) {
    if (how == LOCK_IRQ)
        spinlock_release(&log_lock);
    else if (how == LOCK_RAW)
        __sync_lock_release(&log_lock.locked);
}

static inline void ring_put(char c) {
    ring[head & ring_mask] = c;
    head++;
}

void klog_write(int level, const char *s, size_t n) {
    if (level < 0) level = 0;
    if (level > KLOG_DEBUG) level = KLOG_DEBUG;

    int held = log_lock_take();
    for (size_t i = 0; i < n; i++) {
        if (line_start) {
            ring_put('<');
            ring_put((char)('0' + level));
            ring_put('>');
        }
        ring_put(s[i]);
        line_start = (s[i] == '\n');
    }
    log_unlock(held);
    klog_flush();
}

void klog_set_buffer(char *buf, size_t size) {
    if (buf == NULL || size < KLOG_EARLY_SIZE || (size & (size - 1)) != 0) return;
    int held = log_lock_take();
    uint64_t old_size = ring_mask + 1;
    uint64_t from = head > old_size ? head - old_size : 0;
    for (uint64_t p = from; p < head; p++) {
        buf[p & (size - 1)] = ring[p & ring_mask];
    }
    ring = buf;
    ring_mask = size - 1;
    log_unlock(held);
}

uint64_t klog_head(void) {
    return __atomic_load_n(&head, __ATOMIC_ACQUIRE);
}

uint64_t klog_tail(void) {
    uint64_t h = klog_head();
    uint64_t size = __atomic_load_n(&ring_mask, __ATOMIC_RELAXED) + 1;
    return h > size ? h - size : 0;
}

size_t klog_read(uint64_t *pos, char *out, size_t n) {
    int held = log_lock_take();
    uint64_t size = ring_mask + 1;
    uint64_t tail = head > size ? head - size : 0;
    if (*pos < tail) *pos = tail;
    if (*pos > head) *pos = head;
    size_t count = 0;
    while (count < n && *pos < head) {
        out[count++] = ring[*pos & ring_mask];
        (*pos)++;
    }
    log_unlock(held);
    return count;
}

/* Print one log byte on every console. Owner only. */
static void console_emit(char c) {
    if (cons_resync) {
        if (c == '\n') {
            cons_resync = 0;
            cons_marker = 3;
        }
        return;
    }
    if (cons_marker > 0) {
        /* "<L>": the level is the middle byte */
        if (cons_marker == 2) cons_skip = (c - '0') > console_level;
        cons_marker--;
        return;
    }
    if (!cons_skip) {
        hal_serial_putchar(c);
        if (screen_hook) screen_hook(c);
    }
    if (c == '\n') cons_marker = 3;
}

/* Print every byte between cons_pos and head, a chunk at a time, so the log
 * lock is held only for small copies. Owner only. */
static void console_drain(void) {
    char chunk[128];
    for (;;) {
        uint64_t before = cons_pos;
        size_t n = klog_read(&cons_pos, chunk, sizeof(chunk));
        if (n == 0) return;
        if (cons_pos - n != before) {
            /* The ring overwrote bytes the consoles never saw: resync at a line */
            cons_resync = 1;
            hal_serial_putchar('\n');
        }
        for (size_t i = 0; i < n; i++) console_emit(chunk[i]);
    }
}

int klog_console_try_acquire(void) {
    if (__atomic_load_n(&panicking, __ATOMIC_RELAXED)) return 1;
    return __atomic_exchange_n(&console_owner, 1, __ATOMIC_ACQUIRE) == 0;
}

void klog_console_release(void) {
    if (__atomic_load_n(&panicking, __ATOMIC_RELAXED)) return;
    __atomic_store_n(&console_owner, 0, __ATOMIC_RELEASE);
    klog_flush();
}

void klog_flush(void) {
    if (__atomic_load_n(&panicking, __ATOMIC_RELAXED)) {
        console_drain();   /* the panicking CPU prints, owner or not */
        return;
    }
    /* A writer that failed to take the console while we drained left its bytes
     * for us; checking again after letting go means none are stranded. */
    while (__atomic_load_n(&cons_pos, __ATOMIC_ACQUIRE) != klog_head()) {
        if (__atomic_exchange_n(&console_owner, 1, __ATOMIC_ACQUIRE) != 0) return;
        console_drain();
        __atomic_store_n(&console_owner, 0, __ATOMIC_RELEASE);
    }
}
