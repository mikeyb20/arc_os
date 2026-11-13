/* arc_os — file descriptor tables (fd.h, M1-11)
 *
 * Owns: per-process fd slots, their close-on-exec flags and the table
 * refcount.
 * Invariants: bit n of `open` is set exactly when files[n] holds a File
 * reference; `cloexec` bits are only set for open fds.
 * Locks: t->lock (LOCK_LEVEL_FD_TABLE) around slot changes; fput only after
 * it is released (release may sleep: a pipe's wakeups, a socket's teardown).
 * Cost: lowest free fd by count-trailing-zeros on the inverted bitmap, O(1);
 * fork and exit visit only open fds. */

#include "proc/fd.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"

FdTable *fd_table_new(void) {
    FdTable *t = kmalloc(sizeof(FdTable), GFP_ZERO);
    if (t == NULL) return NULL;
    spin_lock_init(&t->lock, LOCK_LEVEL_FD_TABLE, "fd table");
    t->refcount = 1;
    return t;
}

FdTable *fd_table_dup(FdTable *src) {
    FdTable *t = fd_table_new();
    if (t == NULL || src == NULL) return t;
    uint64_t flags;
    spin_lock_irqsave(&src->lock, &flags);
    t->open = src->open;
    t->cloexec = src->cloexec;
    for (uint64_t m = src->open; m != 0; m &= m - 1) {
        int fd = __builtin_ctzll(m);
        t->files[fd] = fget_ref(src->files[fd]);
    }
    spin_unlock_irqrestore(&src->lock, flags);
    return t;
}

void fd_table_get(FdTable *t) {
    __atomic_add_fetch(&t->refcount, 1, __ATOMIC_RELAXED);
}

void fd_table_put(FdTable *t) {
    if (t == NULL) return;
    if (__atomic_sub_fetch(&t->refcount, 1, __ATOMIC_ACQ_REL) != 0) return;
    fd_close_all(t);
    kfree(t);
}

/* Lowest clear bit of mask at or above min, or -1. */
static int lowest_free(uint64_t mask, int min) {
    uint64_t free = ~mask & (~0ULL << min);
    return free == 0 ? -1 : __builtin_ctzll(free);
}

int fd_install(FdTable *t, File *file, int min_fd, int cloexec) {
    if (min_fd < 0 || min_fd >= MAX_FDS) return -EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&t->lock, &flags);
    int fd = lowest_free(t->open, min_fd);
    if (fd >= 0) {
        t->files[fd] = file;
        t->open |= 1ULL << fd;
        if (cloexec)
            t->cloexec |= 1ULL << fd;
        else
            t->cloexec &= ~(1ULL << fd);
    }
    spin_unlock_irqrestore(&t->lock, flags);
    return fd >= 0 ? fd : -EMFILE;
}

int fd_install_at(FdTable *t, int fd, File *file, int cloexec) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    uint64_t flags;
    spin_lock_irqsave(&t->lock, &flags);
    File *old = (t->open & (1ULL << fd)) ? t->files[fd] : NULL;
    t->files[fd] = file;
    t->open |= 1ULL << fd;
    if (cloexec)
        t->cloexec |= 1ULL << fd;
    else
        t->cloexec &= ~(1ULL << fd);
    spin_unlock_irqrestore(&t->lock, flags);
    fput(old);
    return 0;
}

File *fd_get(FdTable *t, int fd) {
    if (t == NULL || fd < 0 || fd >= MAX_FDS) return NULL;
    uint64_t flags;
    spin_lock_irqsave(&t->lock, &flags);
    File *f = (t->open & (1ULL << fd)) ? fget_ref(t->files[fd]) : NULL;
    spin_unlock_irqrestore(&t->lock, flags);
    return f;
}

/* Empty slot fd and return what it held (NULL if nothing). Lock held. */
static File *slot_take(FdTable *t, int fd) {
    if (!(t->open & (1ULL << fd))) return NULL;
    File *f = t->files[fd];
    t->files[fd] = NULL;
    t->open &= ~(1ULL << fd);
    t->cloexec &= ~(1ULL << fd);
    return f;
}

int fd_close(FdTable *t, int fd) {
    if (t == NULL || fd < 0 || fd >= MAX_FDS) return -EBADF;
    uint64_t flags;
    spin_lock_irqsave(&t->lock, &flags);
    File *f = slot_take(t, fd);
    spin_unlock_irqrestore(&t->lock, flags);
    if (f == NULL) return -EBADF;
    fput(f);
    return 0;
}

int fd_get_cloexec(FdTable *t, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    uint64_t flags;
    spin_lock_irqsave(&t->lock, &flags);
    int r = !(t->open & (1ULL << fd)) ? -EBADF : (int)((t->cloexec >> fd) & 1);
    spin_unlock_irqrestore(&t->lock, flags);
    return r;
}

int fd_set_cloexec(FdTable *t, int fd, int on) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    uint64_t flags;
    spin_lock_irqsave(&t->lock, &flags);
    int r = -EBADF;
    if (t->open & (1ULL << fd)) {
        if (on)
            t->cloexec |= 1ULL << fd;
        else
            t->cloexec &= ~(1ULL << fd);
        r = 0;
    }
    spin_unlock_irqrestore(&t->lock, flags);
    return r;
}

/* Close every fd in mask (taken under the lock, released outside it). */
static void close_mask(FdTable *t, int all) {
    File *taken[MAX_FDS];
    int n = 0;
    uint64_t flags;
    spin_lock_irqsave(&t->lock, &flags);
    for (uint64_t m = all ? t->open : t->cloexec; m != 0; m &= m - 1) {
        taken[n++] = slot_take(t, __builtin_ctzll(m));
    }
    spin_unlock_irqrestore(&t->lock, flags);
    for (int i = 0; i < n; i++) fput(taken[i]);
}

void fd_close_on_exec(FdTable *t) {
    if (t != NULL) close_mask(t, 0);
}

void fd_close_all(FdTable *t) {
    if (t != NULL) close_mask(t, 1);
}
