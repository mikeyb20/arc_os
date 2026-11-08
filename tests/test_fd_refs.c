/* arc_os — Host-side tests for the real kernel/proc/fd.c reference handling
 * (M0-10): fd_close and fd_file_addref take and drop pipe-end and socket
 * references, the one place close, dup, dup2, fork and exit do it. */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARCHOS_LIB_MEM_H         /* host libc memset/memcpy */
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_FS_PIPE_H
#define ARCHOS_NET_SOCKET_H

#include "fs/vfs.h"

static void *kmalloc(size_t size, uint32_t flags) {
    (void)flags;
    return malloc(size);
}
static void kfree(void *p) {
    free(p);
}

/* vfs_close as the kernel does it: clears the file (renamed: test_vfs.c links
 * the real one) */
static int fdr_vfs_close(VfsFile *file) {
    file->node = NULL;
    file->offset = 0;
    file->flags = 0;
    return 0;
}
#define vfs_close fdr_vfs_close

/* Pipe and socket reference counters */
static int pipe_refs, socket_refs[4];
static void pipe_addref(VfsNode *node) {
    (void)node;
    pipe_refs++;
}
static void pipe_close(VfsNode *node) {
    (void)node;
    pipe_refs--;
}
static void socket_addref(int idx) {
    socket_refs[idx]++;
}
static void socket_release(int idx) {
    socket_refs[idx]--;
}

#include "../kernel/proc/fd.c"

static VfsNode pipe_node = {.type = VFS_PIPE};
static VfsNode file_node = {.type = VFS_FILE};

/* Table with fd 0 = regular file, fd 1 = pipe end, fd 2 = socket 3 */
static void setup(FdTable *t) {
    fd_table_init(t);
    pipe_refs = 1;
    memset(socket_refs, 0, sizeof(socket_refs));
    socket_refs[3] = 1;
    fd_alloc(t);
    fd_get(t, 0)->node = &file_node;
    fd_alloc(t);
    fd_get(t, 1)->node = &pipe_node;
    fd_alloc(t);
    VfsFile *s = fd_get(t, 2);
    s->node = NULL;
    s->offset = 3;
    s->flags = VFS_SOCKET;
}

TEST(close_drops_each_kind_once) {
    FdTable t;
    setup(&t);
    fd_close(&t, 0);
    fd_close(&t, 1);
    fd_close(&t, 2);
    ASSERT_EQ(pipe_refs, 0);
    ASSERT_EQ(socket_refs[3], 0);
    ASSERT_TRUE(fd_get(&t, 0) == NULL && fd_get(&t, 1) == NULL && fd_get(&t, 2) == NULL);
    fd_close(&t, 2);                     /* already closed: no second drop */
    fd_close(&t, 99);                    /* out of range: ignored */
    ASSERT_EQ(socket_refs[3], 0);
    return 0;
}

TEST(addref_takes_pipe_and_socket_refs) {
    FdTable t;
    setup(&t);
    fd_file_addref(fd_get(&t, 0));       /* regular file: nothing to count */
    fd_file_addref(fd_get(&t, 1));
    fd_file_addref(fd_get(&t, 2));
    ASSERT_EQ(pipe_refs, 2);
    ASSERT_EQ(socket_refs[3], 2);
    return 0;
}

/* A socket fd is told apart from a file at offset 3 by node == NULL */
TEST(file_with_socket_flags_value_is_not_socket) {
    FdTable t;
    setup(&t);
    VfsFile *f = fd_get(&t, 0);
    f->offset = 3;
    f->flags = VFS_SOCKET;               /* same bits as a socket, but has a node */
    fd_close(&t, 0);
    ASSERT_EQ(socket_refs[3], 1);
    return 0;
}

/* fork: the copy holds its own reference to every pipe end and socket */
TEST(table_dup_takes_refs) {
    FdTable t;
    setup(&t);
    FdTable *c = fd_table_dup(&t);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(pipe_refs, 2);
    ASSERT_EQ(socket_refs[3], 2);
    for (int i = 0; i < MAX_FDS; i++) fd_close(c, i);
    ASSERT_EQ(pipe_refs, 1);
    ASSERT_EQ(socket_refs[3], 1);
    kfree(c);
    return 0;
}

TestCase fd_refs_tests[] = {
    TEST_ENTRY(close_drops_each_kind_once),
    TEST_ENTRY(addref_takes_pipe_and_socket_refs),
    TEST_ENTRY(file_with_socket_flags_value_is_not_socket),
    TEST_ENTRY(table_dup_takes_refs),
};
int fd_refs_test_count = sizeof(fd_refs_tests) / sizeof(fd_refs_tests[0]);
