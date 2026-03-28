/* arc_os libc — directory operations via SYS_READDIR */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* Kernel VfsDirEntry layout — must match kernel/fs/vfs.h */
typedef struct {
    char     name[256];
    uint64_t inode_num;
    uint8_t  type;
} KernelDirEntry;

DIR *opendir(const char *path) {
    DIR *dir = malloc(sizeof(DIR));
    if (!dir) return NULL;
    memset(dir, 0, sizeof(DIR));
    strncpy(dir->path, path, sizeof(dir->path) - 1);

    /* Read all entries via SYS_READDIR */
    KernelDirEntry kentries[64];
    int64_t ret = syscall3(SYS_READDIR, (uint64_t)path,
                           (uint64_t)kentries, 64);
    if (ret < 0) {
        free(dir);
        return NULL;
    }

    dir->count = (int)ret;
    for (int i = 0; i < dir->count; i++) {
        strncpy(dir->entries[i].d_name, kentries[i].name, 255);
        dir->entries[i].d_name[255] = '\0';
        dir->entries[i].d_ino = kentries[i].inode_num;
        dir->entries[i].d_type = kentries[i].type;
    }
    dir->pos = 0;
    return dir;
}

struct dirent *readdir(DIR *dir) {
    if (!dir || dir->pos >= dir->count) return NULL;
    return &dir->entries[dir->pos++];
}

int closedir(DIR *dir) {
    if (dir) free(dir);
    return 0;
}
