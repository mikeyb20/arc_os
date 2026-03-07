#include "proc/fd.h"
#include "lib/mem.h"

void fd_table_init(FdTable *table) {
    memset(table, 0, sizeof(FdTable));
}

int fd_alloc(FdTable *table) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!table->entries[i].in_use) {
            table->entries[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

void fd_free(FdTable *table, int fd) {
    if (fd >= 0 && fd < MAX_FDS) {
        table->entries[fd].in_use = 0;
    }
}

VfsFile *fd_get(FdTable *table, int fd) {
    if (fd < 0 || fd >= MAX_FDS || !table->entries[fd].in_use) {
        return NULL;
    }
    return &table->entries[fd].file;
}
