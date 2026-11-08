/* arc_os libc — directory streams over getdents64 (ADR-0001) */

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "syscall_arch.h"

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return NULL;
    DIR *dir = malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        return NULL;
    }
    dir->fd = fd;
    dir->pos = 0;
    dir->len = 0;
    return dir;
}

struct dirent *readdir(DIR *dir) {
    if (!dir) return NULL;
    if (dir->pos >= dir->len) {
        int64_t n = syscall_ret(syscall3(__NR_getdents64, (uint64_t)dir->fd,
                                         (uint64_t)dir->buf, sizeof(dir->buf)));
        if (n <= 0) return NULL;   /* end of directory, or error (errno set) */
        dir->len = (int)n;
        dir->pos = 0;
    }
    struct dirent *de = (struct dirent *)(dir->buf + dir->pos);
    if (de->d_reclen == 0) return NULL;   /* malformed buffer: stop, don't spin */
    dir->pos += de->d_reclen;
    return de;
}

int closedir(DIR *dir) {
    if (!dir) return -1;
    int ret = close(dir->fd);
    free(dir);
    return ret;
}
