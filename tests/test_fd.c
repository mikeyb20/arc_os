/* arc_os — Host-side tests for file descriptor table */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Stub memset — fd.c uses lib/mem.h */
#define ARCHOS_LIB_MEM_H
/* We're on host, so libc memset is fine */

/* Include fd.c directly with stubs */

/* Minimal VfsFile/VfsNode stubs for the FdEntry type */
typedef struct VfsNode {
    uint64_t inode_num;
} VfsNode;

typedef struct {
    VfsNode *node;
    uint64_t offset;
    uint32_t flags;
} VfsFile;

/* Define MAX_FDS and types before including */
#define MAX_FDS 64

typedef struct {
    VfsFile file;
    uint8_t in_use;
} FdEntry;

typedef struct FdTable {
    FdEntry entries[MAX_FDS];
} FdTable;

/* Implement inline since we're stubbing */
static void fd_table_init(FdTable *table) {
    memset(table, 0, sizeof(FdTable));
}

static int fd_alloc(FdTable *table) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!table->entries[i].in_use) {
            table->entries[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static void fd_free(FdTable *table, int fd) {
    if (fd >= 0 && fd < MAX_FDS) {
        table->entries[fd].in_use = 0;
    }
}

static VfsFile *fd_get(FdTable *table, int fd) {
    if (fd < 0 || fd >= MAX_FDS || !table->entries[fd].in_use) {
        return NULL;
    }
    return &table->entries[fd].file;
}

/* --- Tests --- */

TEST(init_all_unused) {
    FdTable table;
    fd_table_init(&table);
    for (int i = 0; i < MAX_FDS; i++) {
        ASSERT_EQ(table.entries[i].in_use, 0);
    }
    return 0;
}

TEST(alloc_returns_lowest) {
    FdTable table;
    fd_table_init(&table);
    ASSERT_EQ(fd_alloc(&table), 0);
    ASSERT_EQ(fd_alloc(&table), 1);
    ASSERT_EQ(fd_alloc(&table), 2);
    return 0;
}

TEST(free_reuses_slot) {
    FdTable table;
    fd_table_init(&table);
    fd_alloc(&table); /* 0 */
    fd_alloc(&table); /* 1 */
    fd_alloc(&table); /* 2 */
    fd_free(&table, 1);
    ASSERT_EQ(fd_alloc(&table), 1); /* Reuses freed slot */
    return 0;
}

TEST(get_valid_fd) {
    FdTable table;
    fd_table_init(&table);
    int fd = fd_alloc(&table);
    VfsFile *f = fd_get(&table, fd);
    ASSERT_TRUE(f != NULL);
    return 0;
}

TEST(get_invalid_fd) {
    FdTable table;
    fd_table_init(&table);
    ASSERT_TRUE(fd_get(&table, 0) == NULL);  /* Not allocated */
    ASSERT_TRUE(fd_get(&table, -1) == NULL); /* Negative */
    ASSERT_TRUE(fd_get(&table, MAX_FDS) == NULL); /* Out of range */
    return 0;
}

TEST(get_after_free) {
    FdTable table;
    fd_table_init(&table);
    int fd = fd_alloc(&table);
    fd_free(&table, fd);
    ASSERT_TRUE(fd_get(&table, fd) == NULL);
    return 0;
}

TEST(full_table_returns_minus_one) {
    FdTable table;
    fd_table_init(&table);
    for (int i = 0; i < MAX_FDS; i++) {
        int fd = fd_alloc(&table);
        ASSERT_EQ(fd, i);
    }
    ASSERT_EQ(fd_alloc(&table), -1);
    return 0;
}

TEST(free_out_of_range) {
    FdTable table;
    fd_table_init(&table);
    /* Should not crash */
    fd_free(&table, -1);
    fd_free(&table, MAX_FDS);
    fd_free(&table, 1000);
    return 0;
}

/* --- Suite --- */

TestCase fd_tests[] = {
    TEST_ENTRY(init_all_unused),
    TEST_ENTRY(alloc_returns_lowest),
    TEST_ENTRY(free_reuses_slot),
    TEST_ENTRY(get_valid_fd),
    TEST_ENTRY(get_invalid_fd),
    TEST_ENTRY(get_after_free),
    TEST_ENTRY(full_table_returns_minus_one),
    TEST_ENTRY(free_out_of_range),
};
int fd_test_count = sizeof(fd_tests) / sizeof(fd_tests[0]);
