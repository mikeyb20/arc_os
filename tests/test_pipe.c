/* arc_os — Host-side tests for pipe implementation */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* --- Stubs for kernel dependencies --- */

/* Guard out kernel headers */
#define ARCHOS_LIB_MEM_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_PROC_SCHED_H

/* GFP flags */
#define GFP_KERNEL  0x00
#define GFP_ZERO    0x01

/* kmalloc/kfree via libc */
static void *kmalloc(size_t size, uint32_t flags) {
    void *p = malloc(size);
    if (p && (flags & GFP_ZERO)) memset(p, 0, size);
    return p;
}
static void kfree(void *ptr) { free(ptr); }

/* sched_yield — no-op in host tests */
static void sched_yield(void) {}

/* VfsNode/VfsOps types — include vfs.h definitions manually to avoid
 * kernel header dependency chain */
#define VFS_FILE      0
#define VFS_DIRECTORY 1
#define VFS_PIPE      2

#define ENOMEM 12
#define EPIPE  32

typedef struct VfsNode VfsNode;
typedef struct {
    int      (*read)(VfsNode *node, void *buf, uint32_t offset, uint32_t size);
    int      (*write)(VfsNode *node, const void *buf, uint32_t offset, uint32_t size);
    VfsNode *(*lookup)(VfsNode *dir, const char *name);
    VfsNode *(*create)(VfsNode *dir, const char *name, uint8_t type);
    int      (*unlink)(VfsNode *dir, const char *name);
    int      (*readdir)(VfsNode *dir, void *entries, uint32_t max);
    void     (*truncate)(VfsNode *node, uint64_t size);
} VfsOps;

struct VfsNode {
    uint64_t       inode_num;
    uint8_t        type;
    uint64_t       size;
    uint32_t       mode;
    const VfsOps  *ops;
    void          *private_data;
};

/* Now include pipe.c directly */
#define ARCHOS_FS_VFS_H   /* prevent vfs.h re-include */
#define ARCHOS_FS_PIPE_H  /* prevent pipe.h re-include */

#define PIPE_BUF_SIZE 4096

/* Forward declare the public API that pipe.c defines */
int pipe_create(VfsNode **read_node, VfsNode **write_node);
void pipe_close(VfsNode *node);
void pipe_addref(VfsNode *node);

#include "../kernel/fs/pipe.c"

/* --- Tests --- */

TEST(create_returns_zero) {
    VfsNode *r, *w;
    int err = pipe_create(&r, &w);
    ASSERT_EQ(err, 0);
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(w != NULL);
    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(nodes_are_pipe_type) {
    VfsNode *r, *w;
    pipe_create(&r, &w);
    ASSERT_EQ(r->type, VFS_PIPE);
    ASSERT_EQ(w->type, VFS_PIPE);
    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(write_then_read) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    const char *msg = "hello";
    int written = w->ops->write(w, msg, 0, 5);
    ASSERT_EQ(written, 5);

    char buf[16] = {0};
    int rd = r->ops->read(r, buf, 0, 5);
    ASSERT_EQ(rd, 5);
    ASSERT_MEM_EQ(buf, "hello", 5);

    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(read_eof_when_writer_closed) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    /* Write some data, then close writer */
    w->ops->write(w, "ab", 0, 2);
    pipe_close(w);

    /* Read should get data then EOF */
    char buf[16] = {0};
    int rd = r->ops->read(r, buf, 0, 16);
    ASSERT_EQ(rd, 2);
    ASSERT_MEM_EQ(buf, "ab", 2);

    /* Next read should return 0 (EOF) */
    rd = r->ops->read(r, buf, 0, 16);
    ASSERT_EQ(rd, 0);

    pipe_close(r);
    return 0;
}

TEST(write_epipe_when_reader_closed) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    pipe_close(r);

    int written = w->ops->write(w, "x", 0, 1);
    ASSERT_EQ(written, -EPIPE);

    pipe_close(w);
    return 0;
}

TEST(read_ops_has_no_write) {
    VfsNode *r, *w;
    pipe_create(&r, &w);
    ASSERT_TRUE(r->ops->read != NULL);
    ASSERT_TRUE(r->ops->write == NULL);
    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(write_ops_has_no_read) {
    VfsNode *r, *w;
    pipe_create(&r, &w);
    ASSERT_TRUE(w->ops->write != NULL);
    ASSERT_TRUE(w->ops->read == NULL);
    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(fill_buffer_exact) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    /* Fill pipe buffer exactly */
    uint8_t data[PIPE_BUF_SIZE];
    memset(data, 'A', PIPE_BUF_SIZE);
    int written = w->ops->write(w, data, 0, PIPE_BUF_SIZE);
    ASSERT_EQ(written, PIPE_BUF_SIZE);

    /* Read it all back */
    uint8_t out[PIPE_BUF_SIZE];
    int rd = r->ops->read(r, out, 0, PIPE_BUF_SIZE);
    ASSERT_EQ(rd, PIPE_BUF_SIZE);
    ASSERT_MEM_EQ(out, data, PIPE_BUF_SIZE);

    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(addref_prevents_early_free) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    /* Add extra read reference (simulates fork/dup2) */
    pipe_addref(r);

    /* Close original reader — pipe should still be alive */
    pipe_close(r);

    /* Write should still work */
    int written = w->ops->write(w, "x", 0, 1);
    ASSERT_EQ(written, 1);

    /* Read should still work */
    char buf[4] = {0};
    int rd = r->ops->read(r, buf, 0, 1);
    ASSERT_EQ(rd, 1);
    ASSERT_EQ(buf[0], 'x');

    pipe_close(r);  /* second close — reader_count goes to 0 */
    pipe_close(w);
    return 0;
}

TEST(addref_write_end) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    pipe_addref(w);  /* writer_count = 2 */
    pipe_close(w);   /* writer_count = 1 — still alive */

    /* Write should still work since writer_count > 0 */
    int written = w->ops->write(w, "y", 0, 1);
    ASSERT_EQ(written, 1);

    char buf[4] = {0};
    int rd = r->ops->read(r, buf, 0, 1);
    ASSERT_EQ(rd, 1);
    ASSERT_EQ(buf[0], 'y');

    pipe_close(w);  /* writer_count = 0 */
    pipe_close(r);
    return 0;
}

TEST(multiple_small_writes) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    w->ops->write(w, "abc", 0, 3);
    w->ops->write(w, "def", 0, 3);

    char buf[16] = {0};
    int rd = r->ops->read(r, buf, 0, 6);
    ASSERT_EQ(rd, 6);
    ASSERT_MEM_EQ(buf, "abcdef", 6);

    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(partial_read) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    w->ops->write(w, "hello", 0, 5);

    /* Read only 3 bytes */
    char buf[16] = {0};
    int rd = r->ops->read(r, buf, 0, 3);
    ASSERT_EQ(rd, 3);
    ASSERT_MEM_EQ(buf, "hel", 3);

    /* Remaining 2 bytes should still be in pipe */
    rd = r->ops->read(r, buf, 0, 2);
    ASSERT_EQ(rd, 2);
    ASSERT_MEM_EQ(buf, "lo", 2);

    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(zero_length_read) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    w->ops->write(w, "x", 0, 1);
    char buf[4];
    int rd = r->ops->read(r, buf, 0, 0);
    ASSERT_EQ(rd, 0);

    pipe_close(r);
    pipe_close(w);
    return 0;
}

TEST(zero_length_write) {
    VfsNode *r, *w;
    pipe_create(&r, &w);

    int written = w->ops->write(w, "x", 0, 0);
    ASSERT_EQ(written, 0);

    pipe_close(r);
    pipe_close(w);
    return 0;
}

/* --- Suite --- */

TestCase pipe_tests[] = {
    TEST_ENTRY(create_returns_zero),
    TEST_ENTRY(nodes_are_pipe_type),
    TEST_ENTRY(write_then_read),
    TEST_ENTRY(read_eof_when_writer_closed),
    TEST_ENTRY(write_epipe_when_reader_closed),
    TEST_ENTRY(read_ops_has_no_write),
    TEST_ENTRY(write_ops_has_no_read),
    TEST_ENTRY(fill_buffer_exact),
    TEST_ENTRY(addref_prevents_early_free),
    TEST_ENTRY(addref_write_end),
    TEST_ENTRY(multiple_small_writes),
    TEST_ENTRY(partial_read),
    TEST_ENTRY(zero_length_read),
    TEST_ENTRY(zero_length_write),
};
int pipe_test_count = sizeof(pipe_tests) / sizeof(pipe_tests[0]);
