#include "fs/vfs.h"
#include "lib/string.h"

static VfsNode *vfs_root;

void vfs_init(void) {
    vfs_root = NULL;
}

void vfs_set_root(VfsNode *root) {
    vfs_root = root;
}

VfsNode *vfs_get_root(void) {
    return vfs_root;
}

/* Split the next path component from 'path' into 'comp' (max comp_size-1 chars).
 * Returns pointer to the rest of the path, or NULL if no more components. */
static const char *path_next_component(const char *path, char *comp, size_t comp_size) {
    /* Skip leading slashes */
    while (*path == '/') path++;

    if (*path == '\0') return NULL;

    size_t i = 0;
    while (*path != '/' && *path != '\0' && i < comp_size - 1) {
        comp[i++] = *path++;
    }
    comp[i] = '\0';

    /* Skip trailing slashes */
    while (*path == '/') path++;

    return path;
}

/* Resolve parent directory and extract the final component name.
 * Returns the parent VfsNode, or NULL on error. Sets errno_out on failure. */
static VfsNode *vfs_resolve_parent(const char *path, char *name_out, size_t name_size, int *errno_out) {
    if (path == NULL || path[0] != '/' || vfs_root == NULL) {
        *errno_out = EINVAL;
        return NULL;
    }

    /* Find the last '/' to split parent path from name */
    const char *last_slash = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') last_slash = p;
    }

    if (last_slash == NULL) {
        *errno_out = EINVAL;
        return NULL;
    }

    /* Extract the final component name */
    const char *name = last_slash + 1;
    if (*name == '\0') {
        *errno_out = EINVAL;
        return NULL;
    }
    strncpy(name_out, name, name_size);
    name_out[name_size - 1] = '\0';

    /* If parent path is just "/", resolve to root */
    if (last_slash == path) {
        return vfs_root;
    }

    /* Resolve the parent path by walking components up to last_slash */
    VfsNode *node = vfs_root;
    const char *cursor = path;
    char comp[VFS_NAME_MAX];

    while (cursor < last_slash) {
        cursor = path_next_component(cursor, comp, sizeof(comp));
        if (cursor == NULL) break;

        if (node->type != VFS_DIRECTORY || node->ops == NULL || node->ops->lookup == NULL) {
            *errno_out = ENOTDIR;
            return NULL;
        }

        VfsNode *child = node->ops->lookup(node, comp);
        if (child == NULL) {
            *errno_out = ENOENT;
            return NULL;
        }
        node = child;

        /* If we've consumed up to or past last_slash, stop */
        if (cursor >= last_slash) break;
    }

    if (node->type != VFS_DIRECTORY) {
        *errno_out = ENOTDIR;
        return NULL;
    }

    return node;
}

VfsNode *vfs_resolve(const char *path) {
    if (path == NULL || path[0] != '/' || vfs_root == NULL) {
        return NULL;
    }

    VfsNode *node = vfs_root;
    const char *cursor = path;
    char comp[VFS_NAME_MAX];

    while ((cursor = path_next_component(cursor, comp, sizeof(comp))) != NULL) {
        if (node->type != VFS_DIRECTORY || node->ops == NULL || node->ops->lookup == NULL) {
            return NULL;
        }

        VfsNode *child = node->ops->lookup(node, comp);
        if (child == NULL) {
            return NULL;
        }
        node = child;
    }

    return node;
}

int vfs_open(const char *path, uint32_t flags, VfsFile *out) {
    if (out == NULL) return -EINVAL;

    VfsNode *node = vfs_resolve(path);

    if (node == NULL) {
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }

        /* Create the file: resolve parent, then call create */
        int err = 0;
        char name[VFS_NAME_MAX];
        VfsNode *parent = vfs_resolve_parent(path, name, sizeof(name), &err);
        if (parent == NULL) return -err;

        if (parent->ops == NULL || parent->ops->create == NULL) {
            return -EINVAL;
        }

        node = parent->ops->create(parent, name, VFS_FILE);
        if (node == NULL) return -ENOMEM;
    }

    if (node->type == VFS_DIRECTORY) {
        return -EISDIR;
    }

    /* Truncate if requested */
    if ((flags & O_TRUNC) && node->ops && node->ops->truncate) {
        node->ops->truncate(node, 0);
        node->size = 0;
    }

    out->node = node;
    out->flags = flags;
    out->offset = (flags & O_APPEND) ? node->size : 0;

    return VFS_OK;
}

int vfs_close(VfsFile *file) {
    if (file == NULL) return -EINVAL;
    file->node = NULL;
    file->offset = 0;
    file->flags = 0;
    return VFS_OK;
}

int vfs_read(VfsFile *file, void *buf, uint32_t size) {
    if (file == NULL || file->node == NULL || buf == NULL) return -EINVAL;
    if ((file->flags & O_ACCMODE) == O_WRONLY) return -EINVAL;

    VfsNode *node = file->node;
    if (node->ops == NULL || node->ops->read == NULL) return -EINVAL;

    int n = node->ops->read(node, buf, (uint32_t)file->offset, size);
    if (n > 0) {
        file->offset += (uint64_t)n;
    }
    return n;
}

int vfs_write(VfsFile *file, const void *buf, uint32_t size) {
    if (file == NULL || file->node == NULL || buf == NULL) return -EINVAL;
    if ((file->flags & O_ACCMODE) == O_RDONLY) return -EINVAL;

    VfsNode *node = file->node;
    if (node->ops == NULL || node->ops->write == NULL) return -EINVAL;

    /* Append mode: always write at end */
    if (file->flags & O_APPEND) {
        file->offset = node->size;
    }

    int n = node->ops->write(node, buf, (uint32_t)file->offset, size);
    if (n > 0) {
        file->offset += (uint64_t)n;
    }
    return n;
}

int vfs_seek(VfsFile *file, int64_t offset, int whence) {
    if (file == NULL || file->node == NULL) return -EINVAL;

    int64_t new_offset;
    switch (whence) {
    case SEEK_SET:
        new_offset = offset;
        break;
    case SEEK_CUR:
        new_offset = (int64_t)file->offset + offset;
        break;
    case SEEK_END:
        new_offset = (int64_t)file->node->size + offset;
        break;
    default:
        return -EINVAL;
    }

    if (new_offset < 0) return -EINVAL;

    file->offset = (uint64_t)new_offset;
    return VFS_OK;
}

int vfs_stat(const char *path, VfsStat *out) {
    if (out == NULL) return -EINVAL;

    VfsNode *node = vfs_resolve(path);
    if (node == NULL) return -ENOENT;

    out->inode_num = node->inode_num;
    out->type = node->type;
    out->size = node->size;
    out->mode = node->mode;
    return VFS_OK;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    if (path == NULL || vfs_root == NULL) return -EINVAL;

    /* Check if it already exists */
    VfsNode *existing = vfs_resolve(path);
    if (existing != NULL) return -EEXIST;

    /* Resolve parent and get name */
    int err = 0;
    char name[VFS_NAME_MAX];
    VfsNode *parent = vfs_resolve_parent(path, name, sizeof(name), &err);
    if (parent == NULL) return -err;

    if (parent->ops == NULL || parent->ops->create == NULL) {
        return -EINVAL;
    }

    VfsNode *dir = parent->ops->create(parent, name, VFS_DIRECTORY);
    if (dir == NULL) return -ENOMEM;

    dir->mode = mode;
    return VFS_OK;
}

int vfs_readdir(const char *path, VfsDirEntry *entries, uint32_t max) {
    if (entries == NULL) return -EINVAL;

    VfsNode *node = vfs_resolve(path);
    if (node == NULL) return -ENOENT;
    if (node->type != VFS_DIRECTORY) return -ENOTDIR;
    if (node->ops == NULL || node->ops->readdir == NULL) return -EINVAL;

    return node->ops->readdir(node, entries, max);
}

int vfs_unlink(const char *path) {
    if (path == NULL || vfs_root == NULL) return -EINVAL;

    int err = 0;
    char name[VFS_NAME_MAX];
    VfsNode *parent = vfs_resolve_parent(path, name, sizeof(name), &err);
    if (parent == NULL) return -err;

    if (parent->ops == NULL || parent->ops->unlink == NULL) {
        return -EINVAL;
    }

    return parent->ops->unlink(parent, name);
}
