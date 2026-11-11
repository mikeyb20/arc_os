/*
 * arcfile.c — durable-file helpers (arcfile.h). No state.
 */
#include "arcfile.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

bool arcfile_make_dir(const char *path) {
    struct stat st;
    if (mkdir(path, 0755) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    return true;
}

bool arcfile_sync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool ok = fsync(fd) == 0;
    int saved = errno;
    close(fd);
    errno = saved;
    return ok;
}
