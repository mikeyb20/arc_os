#include "fs/pipe.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "proc/waitqueue.h"

/* Internal pipe structure — single allocation, two embedded VfsNodes */
typedef struct PipeNode {
    VfsNode  read_vnode;
    VfsNode  write_vnode;
    uint8_t  buf[PIPE_BUF_SIZE];
    volatile uint32_t head;          /* write position */
    volatile uint32_t tail;          /* read position */
    volatile uint32_t reader_count;  /* >0 = readers exist */
    volatile uint32_t writer_count;  /* >0 = writers exist */
    Spinlock  lock;                  /* condition lock */
    WaitQueue readers_wq;            /* writers wake readers */
    WaitQueue writers_wq;            /* readers wake writers */
} PipeNode;

/* Cast from VfsNode.private_data to PipeNode */
static PipeNode *to_pipe(VfsNode *node) {
    return (PipeNode *)node->private_data;
}

/* --- VfsOps callbacks --- */

static int pipe_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size) {
    (void)offset;
    PipeNode *pipe = to_pipe(node);
    uint32_t total = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (total < size) {
        /* Data available? */
        if (pipe->head != pipe->tail) {
            dst[total++] = pipe->buf[pipe->tail % PIPE_BUF_SIZE];
            pipe->tail++;
        } else if (pipe->writer_count == 0) {
            /* EOF — no writers left */
            break;
        } else {
            /* Buffer empty, writers exist — sleep until woken */
            if (total > 0) break;  /* Return what we have */
            spinlock_acquire(&pipe->lock);
            if (pipe->head == pipe->tail && pipe->writer_count > 0) {
                wq_sleep(&pipe->readers_wq, &pipe->lock);
            } else {
                spinlock_release(&pipe->lock);
            }
        }
    }
    if (total > 0) wq_wake(&pipe->writers_wq);
    return (int)total;
}

static int pipe_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size) {
    (void)offset;
    PipeNode *pipe = to_pipe(node);
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t total = 0;

    while (total < size) {
        if (pipe->reader_count == 0) {
            return total > 0 ? (int)total : -EPIPE;
        }
        if (pipe->head - pipe->tail < PIPE_BUF_SIZE) {
            pipe->buf[pipe->head % PIPE_BUF_SIZE] = src[total++];
            pipe->head++;
        } else {
            /* Buffer full — sleep until woken */
            spinlock_acquire(&pipe->lock);
            if (pipe->head - pipe->tail >= PIPE_BUF_SIZE && pipe->reader_count > 0) {
                wq_sleep(&pipe->writers_wq, &pipe->lock);
            } else {
                spinlock_release(&pipe->lock);
            }
        }
    }
    if (total > 0) wq_wake(&pipe->readers_wq);
    return (int)total;
}

static const VfsOps pipe_read_ops = {
    .read = pipe_read,
};

static const VfsOps pipe_write_ops = {
    .write = pipe_write,
};

/* --- Public API --- */

int pipe_create(VfsNode **read_node, VfsNode **write_node) {
    PipeNode *pipe = kmalloc(sizeof(PipeNode), GFP_ZERO);
    if (pipe == NULL) return -ENOMEM;

    pipe->read_vnode.type = VFS_PIPE;
    pipe->read_vnode.ops = &pipe_read_ops;
    pipe->read_vnode.private_data = pipe;

    pipe->write_vnode.type = VFS_PIPE;
    pipe->write_vnode.ops = &pipe_write_ops;
    pipe->write_vnode.private_data = pipe;

    pipe->reader_count = 1;
    pipe->writer_count = 1;
    wq_init(&pipe->readers_wq);
    wq_init(&pipe->writers_wq);

    *read_node = &pipe->read_vnode;
    *write_node = &pipe->write_vnode;
    return 0;
}

void pipe_close(VfsNode *node) {
    PipeNode *pipe = to_pipe(node);
    if (node == &pipe->read_vnode) {
        pipe->reader_count--;
        wq_wake_all(&pipe->writers_wq);
    } else {
        pipe->writer_count--;
        wq_wake_all(&pipe->readers_wq);
    }
    if (pipe->reader_count == 0 && pipe->writer_count == 0) {
        kfree(pipe);
    }
}

void pipe_addref(VfsNode *node) {
    PipeNode *pipe = to_pipe(node);
    if (node == &pipe->read_vnode) {
        pipe->reader_count++;
    } else {
        pipe->writer_count++;
    }
}
