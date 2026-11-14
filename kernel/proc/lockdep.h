#ifndef ARCHOS_PROC_LOCKDEP_H
#define ARCHOS_PROC_LOCKDEP_H

/* Lock levels and the debug-build lock-order checker (M1-05).
 *
 * Every spinlock and sleeping lock has a level; a lock may be taken only while
 * every lock already held has a lower level (outermost = lowest). Two locks of
 * the same level may be held together only when both carry a subkey and the
 * new one's is higher (run queues: CPU-index order). The levels and the rules
 * are written up in docs/kernel/locking.md, which follows work-blocks.md §7.
 *
 * Debug builds (ARC_DEBUG=1) check every acquisition and panic on an
 * out-of-order one, on taking a sleeping lock or sleeping while a spinlock is
 * held, and on taking the kernel lock with any other lock held. Release builds
 * compile the checks away. */

#include <stdint.h>

/* Levels: work-blocks.md §7, numbered with gaps. Lower = taken first. */
#define LOCK_LEVEL_KERNEL        0   /* big kernel lock (interim, proc/kernel_lock.h) */
#define LOCK_LEVEL_DEVICE        5   /* sleeping: device model (bus lists, probe/remove/suspend; M1-09) */
#define LOCK_LEVEL_FILE_POS      7   /* sleeping: an open file's position (M1-11) */
#define LOCK_LEVEL_RENAME        8   /* sleeping: one rename per filesystem (M1-11) */
#define LOCK_LEVEL_INODE         10   /* sleeping: write, truncate, rename, directories (M1-11) */
#define LOCK_LEVEL_INODE_2       11   /* sleeping: rename's second directory (M1-11) */
#define LOCK_LEVEL_INODE_CHILD   12   /* sleeping: a directory being removed or replaced (M1-11) */
#define LOCK_LEVEL_MM            20   /* sleeping read-write: mmap, munmap, faults (M1-07) */
#define LOCK_LEVEL_PAGE          30   /* sleeping, per page (M1-12) */
#define LOCK_LEVEL_MAPPING       40   /* sleeping: VMAs mapping a file (M1-12) */
#define LOCK_LEVEL_FS_IO         45   /* sleeping: a filesystem's device reads (FAT32 volume; M1-11) */
#define LOCK_LEVEL_PAGE_TABLE    50   /* spinlock, per mm; today the kernel page tables */
#define LOCK_LEVEL_PAGE_CACHE    60   /* spinlock, per address space (M1-12) */
#define LOCK_LEVEL_MOUNT         62   /* spinlock: adding to the mount table (M1-11) */
#define LOCK_LEVEL_DCACHE        63   /* spinlock: the dentry cache (M1-11) */
#define LOCK_LEVEL_ICACHE        64   /* spinlock: the inode cache (M1-11) */
#define LOCK_LEVEL_FD_TABLE      65   /* spinlock: one fd table (M1-11) */
#define LOCK_LEVEL_SIGNAL        70   /* thread-group and signal (M1-16) */
/* LOCK_LEVEL_OBJECT: one object's state, the lock passed to wq_sleep (pipe,
 * socket, tty, wait4, mutex/semaphore guards) */
#define LOCK_LEVEL_OBJECT        75
#define LOCK_LEVEL_OBJECT_TABLE  76   /* a table of objects looked up under an object lock (sockets) */
#define LOCK_LEVEL_WAITQUEUE     80   /* wait-queue locks */
#define LOCK_LEVEL_RUNQUEUE      90   /* run queues: interrupts off; two only in CPU-index order */
#define LOCK_LEVEL_IRQ_DESC      95   /* IRQ descriptors: request, free, affinity, /proc/interrupts (M1-09) */
#define LOCK_LEVEL_IRQ_VECTOR    96   /* per-CPU device vector tables (M1-09) */
#define LOCK_LEVEL_PCI_CONFIG    97   /* PCI config space ports 0xCF8/0xCFC (M1-09) */
#define LOCK_LEVEL_SLAB_REGISTRY 99   /* the list of slab caches */
#define LOCK_LEVEL_SLAB          100   /* one slab cache */
#define LOCK_LEVEL_BUDDY         101   /* buddy allocator (the only lock taken under a slab lock) */
#define LOCK_LEVEL_LOG           109   /* kernel log ring: kprintf works under any lock */

/* Spinlocks one CPU may hold at once (deepest nesting seen is about 5) */
#define LOCKDEP_MAX_HELD       16
/* Sleeping locks one thread may hold at once */
#define LOCKDEP_MAX_SLEEP_HELD 4

struct Spinlock;

/* Spinlocks held on one CPU, innermost last (lives in the CPU's run queue). */
typedef struct LockdepCpu {
    const struct Spinlock *held[LOCKDEP_MAX_HELD];
    uint32_t depth;
} LockdepCpu;

/* Sleeping locks one thread holds (lives in the Thread). */
typedef struct LockdepThread {
    uint16_t level[LOCKDEP_MAX_SLEEP_HELD];
    uint8_t depth;
} LockdepThread;

/* Checker core, no global state (host-tested). Returns NULL if a lock of
 * (level, subkey) may be taken now, else a short reason. */
const char *lockdep_check_order(const LockdepCpu *cpu, const LockdepThread *thr, uint16_t level, uint16_t subkey);

/* Checker core for the kernel lock (host-tested): NULL if it may be taken
 * now. At syscall entry (resuming 0) nothing may be held; taking it back
 * after a sleep (resuming 1) may happen under sleeping locks, not spinlocks. */
const char *lockdep_check_kernel_lock(const LockdepCpu *cpu, const LockdepThread *thr, int resuming);

/* Test mode only (suite "lockorder"): take two spinlocks in the wrong order.
 * A debug build panics with "LOCK ORDER VIOLATION"; a release build says the
 * checker is off and returns. */
void lockdep_test_inversion(void);

#ifndef ARC_LOCKDEP
#if defined(ARC_DEBUG) && ARC_DEBUG && !defined(ARC_HOST_TEST)
#define ARC_LOCKDEP 1
#else
#define ARC_LOCKDEP 0
#endif
#endif

#if ARC_LOCKDEP
/* Spinlock hooks (spinlock.h): check, then record / forget the lock. A
 * trylock (trylock 1) is recorded without the order check. */
void lockdep_acquire(const struct Spinlock *lock, int trylock);
void lockdep_release(const struct Spinlock *lock);
/* Sleeping lock hooks (mutex.c). */
void lockdep_sleep_acquire(uint16_t level, const char *name);
void lockdep_sleep_release(uint16_t level);
/* About to sleep (schedule away while blocking): no spinlock may be held. */
void lockdep_assert_can_sleep(const char *what);
/* About to take the kernel lock: no other lock may be held at syscall entry
 * (resuming 0); when taking it back after a sleep (resuming 1) only
 * spinlocks count. */
void lockdep_kernel_lock(int resuming);
#else
static inline void lockdep_acquire(const struct Spinlock *lock, int trylock) {
    (void)lock;
    (void)trylock;
}
static inline void lockdep_release(const struct Spinlock *lock) {
    (void)lock;
}
static inline void lockdep_sleep_acquire(uint16_t level, const char *name) {
    (void)level;
    (void)name;
}
static inline void lockdep_sleep_release(uint16_t level) {
    (void)level;
}
static inline void lockdep_assert_can_sleep(const char *what) {
    (void)what;
}
static inline void lockdep_kernel_lock(int resuming) {
    (void)resuming;
}
#endif

#endif /* ARCHOS_PROC_LOCKDEP_H */
