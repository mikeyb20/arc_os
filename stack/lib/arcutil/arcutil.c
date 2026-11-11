/*
 * arcutil.c — durable files, directory removal and RAM size (arcutil.h). SHA-256 is in sha256.c.
 *
 * arcutil_remove_tree walks by path in one PATH_MAX buffer (open, getdents, lstat, unlink, rmdir: the
 * syscalls the inventory already has), one open directory per level; symlinks are unlinked, never
 * followed (lstat).
 */
#define _GNU_SOURCE
#include "arcutil.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>

bool arcutil_make_dir(const char *path) {
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

bool arcutil_make_dirs(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/' && buf[i - 1] != '/') {
            buf[i] = '\0';
            bool ok = arcutil_make_dir(buf);
            buf[i] = '/';
            if (!ok) {
                return false;
            }
        }
    }
    return arcutil_make_dir(buf);
}

bool arcutil_sync_dir(const char *path) {
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

/* parent_of: the directory part of `path` ("." if none) into buf. */
static bool parent_of(const char *path, char *buf, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return snprintf(buf, cap, ".") < (int)cap;
    }
    size_t n = slash == path ? 1 : (size_t)(slash - path);
    if (n >= cap) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(buf, path, n);
    buf[n] = '\0';
    return true;
}

bool arcutil_write_durable(const char *path, const void *data, size_t len) {
    char tmp[PATH_MAX];
    char dir[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) || !parent_of(path, dir, sizeof(dir))) {
        errno = ENAMETOOLONG;
        return false;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const char *p = data;
    bool ok = true;
    for (size_t done = 0; ok && done < len;) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        ok = n > 0;
        done += ok ? (size_t)n : 0;
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(tmp, path) == 0 && arcutil_sync_dir(dir);
    if (!ok) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
    }
    return ok;
}

/* remove_path: remove path[0..len) (in a PATH_MAX buffer that is extended in place) and everything under it. */
static bool remove_path(char *path, size_t len) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 || errno == ENOENT;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return false;
    }
    bool ok = true;
    for (struct dirent *e; ok && (e = readdir(d)) != NULL;) {
        size_t n = strlen(e->d_name);
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (len + 1 + n >= PATH_MAX) {
            errno = ENAMETOOLONG;
            ok = false;
            break;
        }
        path[len] = '/';
        memcpy(path + len + 1, e->d_name, n + 1);
        ok = remove_path(path, len + 1 + n);
        path[len] = '\0';
    }
    int saved = errno;
    closedir(d);
    errno = saved;
    return ok && (rmdir(path) == 0 || errno == ENOENT);
}

bool arcutil_remove_tree(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(buf, path, len + 1);
    return remove_path(buf, len);
}

uint64_t arcutil_ram_mib(void) {
    struct sysinfo si;
    if (sysinfo(&si) != 0) {
        return 0;
    }
    return (uint64_t)si.totalram * si.mem_unit / (1024u * 1024u);
}
