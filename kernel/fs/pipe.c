/* arc_os — pipes (pipe.h, M1-11)
 *
 * Owns: the ring buffer, reader/writer counts and wait queues of a pipe. A
 * pipe is one inode on the anonymous superblock (S_IFIFO) with two Files,
 * the read end and the write end; dup and fork share those Files, so the
 * counts here are of Files, changed only when one is released.
 *
 * Invariants: head - tail is the number of bytes buffered (both only grow,
 * mod 2^32); a reader moves tail, a writer head, each only under `lock`.
 * Bytes in [tail, head) belong to readers and [head, tail + size) to
 * writers, so either side copies its part with `lock` dropped (user memory
 * may fault) while the other side works on its own part.
 *
 * Locks: rd_mutex serialises readers and wr_mutex writers (sleeping,
 * LOCK_LEVEL_INODE: the pipe's own inode lock, taken before any user copy);
 * `lock` (LOCK_LEVEL_OBJECT) guards head, tail and the counts and is the
 * condition lock of both wait queues. A reader that copies to a bad user
 * buffer has not moved tail yet, so the bytes stay in the pipe (nothing is
 * lost on -EFAULT).
 *
 * Cost: O(bytes) per transfer, two copies at most per wrap-around; no
 * allocation after creation. */

#include "fs/pipe.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "lib/mem.h"
#include "proc/waitqueue.h"
#include "proc/process.h"

_Static_assert(PIPE_BUF_SIZE == 4096, "a pipe buffer is one page");

typedef struct Pipe {
    uint8_t *buf;            /* one page */
    struct page *page;
    uint32_t head;           /* next byte written */
    uint32_t tail;           /* next byte read */
    uint32_t readers;        /* read-end Files alive */
    uint32_t writers;        /* write-end Files alive */
    Spinlock lock;
    Mutex rd_mutex;
    Mutex wr_mutex;
    WaitQueue readers_wq;    /* writers wake readers */
    WaitQueue writers_wq;    /* readers wake writers */
} Pipe;

static uint64_t pipe_ino = 1;

static Pipe *to_pipe(const File *file) {
    return (Pipe *)file->inode->priv;
}

/* Copy n bytes of the ring at index `at` (mod size) out to b at off. */
static int ring_out(Pipe *p, uint32_t at, const IoBuf *b, size_t off, size_t n) {
    size_t first = PIPE_BUF_SIZE - (at % PIPE_BUF_SIZE);
    if (first > n) first = n;
    int err = iob_copy_out(b, off, p->buf + at % PIPE_BUF_SIZE, first);
    if (err == 0 && n > first) err = iob_copy_out(b, off + first, p->buf, n - first);
    return err;
}

/* Copy n bytes from b at off into the ring at index `at`. */
static int ring_in(Pipe *p, uint32_t at, const IoBuf *b, size_t off, size_t n) {
    size_t first = PIPE_BUF_SIZE - (at % PIPE_BUF_SIZE);
    if (first > n) first = n;
    int err = iob_copy_in(p->buf + at % PIPE_BUF_SIZE, b, off, first);
    if (err == 0 && n > first) err = iob_copy_in(p->buf, b, off + first, n - first);
    return err;
}

static int64_t pipe_read(File *file, const IoBuf *buf, loff_t *pos) {
    (void)pos;
    Pipe *p = to_pipe(file);
    mutex_lock(&p->rd_mutex);
    uint64_t flags;
    spin_lock_irqsave(&p->lock, &flags);
    while (p->head == p->tail) {
        if (p->writers == 0 || (file->flags & O_NONBLOCK)) {
            int64_t r = p->writers == 0 ? 0 : -EAGAIN;   /* end of file, or nothing yet */
            spin_unlock_irqrestore(&p->lock, flags);
            mutex_unlock(&p->rd_mutex);
            return r;
        }
        wq_sleep(&p->readers_wq, &p->lock, flags);
        spin_lock_irqsave(&p->lock, &flags);
    }
    uint32_t tail = p->tail;
    size_t n = p->head - tail;
    spin_unlock_irqrestore(&p->lock, flags);

    if (n > buf->len) n = buf->len;
    int err = ring_out(p, tail, buf, 0, n);
    if (err == 0) {
        spin_lock_irqsave(&p->lock, &flags);
        p->tail = tail + (uint32_t)n;
        spin_unlock_irqrestore(&p->lock, flags);
        wq_wake(&p->writers_wq);
    }
    mutex_unlock(&p->rd_mutex);
    return err != 0 ? err : (int64_t)n;
}

static int64_t pipe_write(File *file, const IoBuf *buf, loff_t *pos) {
    (void)pos;
    Pipe *p = to_pipe(file);
    size_t done = 0;
    int64_t err = 0;
    mutex_lock(&p->wr_mutex);
    while (done < buf->len) {
        uint64_t flags;
        spin_lock_irqsave(&p->lock, &flags);
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            err = -EPIPE;
            break;
        }
        uint32_t head = p->head;
        size_t space = PIPE_BUF_SIZE - (head - p->tail);
        /* POSIX: a write of up to PIPE_BUF bytes goes in whole or not at all */
        size_t need = buf->len <= PIPE_BUF_SIZE ? buf->len - done : 1;
        if (space < need) {
            if (file->flags & O_NONBLOCK) {
                spin_unlock_irqrestore(&p->lock, flags);
                err = -EAGAIN;
                break;
            }
            wq_sleep(&p->writers_wq, &p->lock, flags);
            continue;
        }
        spin_unlock_irqrestore(&p->lock, flags);

        size_t n = buf->len - done < space ? buf->len - done : space;
        err = ring_in(p, head, buf, done, n);
        if (err != 0) break;
        spin_lock_irqsave(&p->lock, &flags);
        p->head = head + (uint32_t)n;
        spin_unlock_irqrestore(&p->lock, flags);
        wq_wake(&p->readers_wq);
        done += n;
    }
    mutex_unlock(&p->wr_mutex);
    return done > 0 ? (int64_t)done : err;
}

static void pipe_release(Inode *inode, File *file) {
    Pipe *p = (Pipe *)inode->priv;
    int reader = (file->flags & O_ACCMODE) == O_RDONLY;
    uint64_t flags;
    spin_lock_irqsave(&p->lock, &flags);
    if (reader)
        p->readers--;
    else
        p->writers--;
    int last = p->readers == 0 && p->writers == 0;
    spin_unlock_irqrestore(&p->lock, flags);
    if (!last) {
        /* The other side sees EOF (no writers) or EPIPE (no readers) */
        wq_wake_all(reader ? &p->writers_wq : &p->readers_wq);
        return;
    }
    free_pages(p->page, 0);
    kfree(p);
    inode->priv = NULL;
}

static const FileOps pipe_read_fops = {
    .fflags = FOP_STREAM,
    .read = pipe_read,
    .release = pipe_release,
};

static const FileOps pipe_write_fops = {
    .fflags = FOP_STREAM,
    .write = pipe_write,
    .release = pipe_release,
};

int pipe_create(File **rd, File **wr, uint32_t extra_flags) {
    Pipe *p = kmalloc(sizeof(Pipe), GFP_ZERO);
    if (p == NULL) return -ENOMEM;
    p->page = alloc_pages(0, GFP_KERNEL);
    Inode *inode = p->page != NULL
                       ? inode_new(vfs_anon_sb(), __atomic_fetch_add(&pipe_ino, 1, __ATOMIC_RELAXED), S_IFIFO | 0600)
                       : NULL;
    if (inode == NULL) {
        if (p->page != NULL) free_pages(p->page, 0);
        kfree(p);
        return -ENOMEM;
    }
    p->buf = page_to_virt(p->page);
    spin_lock_init(&p->lock, LOCK_LEVEL_OBJECT, "pipe");
    mutex_init(&p->rd_mutex, LOCK_LEVEL_INODE);
    mutex_init(&p->wr_mutex, LOCK_LEVEL_INODE);
    wq_init(&p->readers_wq);
    wq_init(&p->writers_wq);
    inode->priv = p;
    Process *cur = proc_current();
    if (cur != NULL) {
        inode->uid = cur->euid;
        inode->gid = cur->egid;
    }
    extra_flags &= O_NONBLOCK;
    *rd = file_new(inode, &pipe_read_fops, O_RDONLY | extra_flags);
    *wr = *rd != NULL ? file_new(igrab(inode), &pipe_write_fops, O_WRONLY | extra_flags) : NULL;
    if (*wr == NULL) {
        /* Nothing released yet: undo by hand */
        if (*rd != NULL) {
            (*rd)->fop = NULL;
            fput(*rd);   /* drops the first inode reference */
            iput(inode); /* the igrab */
        } else {
            iput(inode);
        }
        free_pages(p->page, 0);
        kfree(p);
        return -ENOMEM;
    }
    p->readers = 1;
    p->writers = 1;
    return 0;
}
