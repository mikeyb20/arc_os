#include "fs/ramfs.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "lib/string.h"

#define RAMFS_NAME_MAX     255
#define RAMFS_INIT_CAP     512    /* Initial file data capacity */
#define RAMFS_MAX_CHILDREN 128    /* Max entries per directory */

typedef struct RamfsDirEntry {
    char name[RAMFS_NAME_MAX + 1];
    struct RamfsNode *node;
} RamfsDirEntry;

typedef struct RamfsNode {
    VfsNode          vnode;        /* Embedded VFS node */
    uint8_t         *data;         /* File content (NULL for dirs) */
    uint32_t         capacity;     /* Allocated data capacity */
    RamfsDirEntry   *children;     /* Directory children array (NULL for files) */
    uint32_t         num_children;
} RamfsNode;

static uint64_t next_inode = 1;

/* Forward declarations */
static int ramfs_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size);
static int ramfs_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size);
static VfsNode *ramfs_lookup(VfsNode *dir, const char *name);
static VfsNode *ramfs_create(VfsNode *dir, const char *name, uint8_t type);
static int ramfs_unlink(VfsNode *dir, const char *name);
static int ramfs_readdir(VfsNode *dir, VfsDirEntry *entries, uint32_t max);
static void ramfs_truncate(VfsNode *node, uint64_t size);

static const VfsOps ramfs_ops = {
    .read     = ramfs_read,
    .write    = ramfs_write,
    .lookup   = ramfs_lookup,
    .create   = ramfs_create,
    .unlink   = ramfs_unlink,
    .readdir  = ramfs_readdir,
    .truncate = ramfs_truncate,
};

/* Get the RamfsNode from a VfsNode (they share the same address) */
static RamfsNode *to_ramfs(VfsNode *node) {
    return (RamfsNode *)node;
}

static RamfsNode *ramfs_alloc_node(uint8_t type) {
    RamfsNode *rn = kmalloc(sizeof(RamfsNode), GFP_ZERO);
    if (rn == NULL) return NULL;

    rn->vnode.inode_num = next_inode++;
    rn->vnode.type = type;
    rn->vnode.size = 0;
    rn->vnode.mode = 0;
    rn->vnode.ops = &ramfs_ops;
    rn->vnode.private_data = rn;

    if (type == VFS_DIRECTORY) {
        rn->children = kmalloc(sizeof(RamfsDirEntry) * RAMFS_MAX_CHILDREN, GFP_ZERO);
        if (rn->children == NULL) {
            kfree(rn);
            return NULL;
        }
        rn->num_children = 0;
    }

    return rn;
}

static int ramfs_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size) {
    RamfsNode *rn = to_ramfs(node);
    if (node->type != VFS_FILE) return -EISDIR;
    if (offset >= node->size) return 0;

    uint32_t avail = (uint32_t)(node->size - offset);
    uint32_t to_read = (size < avail) ? size : avail;
    memcpy(buf, rn->data + offset, to_read);
    return (int)to_read;
}

static int ramfs_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size) {
    RamfsNode *rn = to_ramfs(node);
    if (node->type != VFS_FILE) return -EISDIR;

    uint32_t end = offset + size;

    /* Grow buffer if needed */
    if (end > rn->capacity) {
        uint32_t new_cap = rn->capacity ? rn->capacity : RAMFS_INIT_CAP;
        while (new_cap < end) {
            new_cap *= 2;
        }
        uint8_t *new_data = krealloc(rn->data, new_cap);
        if (new_data == NULL) return -ENOMEM;
        /* Zero the gap between old capacity and new capacity */
        if (new_cap > rn->capacity) {
            memset(new_data + rn->capacity, 0, new_cap - rn->capacity);
        }
        rn->data = new_data;
        rn->capacity = new_cap;
    }

    memcpy(rn->data + offset, buf, size);

    if (end > node->size) {
        node->size = end;
    }

    return (int)size;
}

static VfsNode *ramfs_lookup(VfsNode *dir, const char *name) {
    RamfsNode *rn = to_ramfs(dir);
    if (dir->type != VFS_DIRECTORY) return NULL;

    for (uint32_t i = 0; i < rn->num_children; i++) {
        if (strcmp(rn->children[i].name, name) == 0) {
            return &rn->children[i].node->vnode;
        }
    }
    return NULL;
}

static VfsNode *ramfs_create(VfsNode *dir, const char *name, uint8_t type) {
    RamfsNode *parent = to_ramfs(dir);
    if (dir->type != VFS_DIRECTORY) return NULL;
    if (parent->num_children >= RAMFS_MAX_CHILDREN) return NULL;

    /* Check for duplicate */
    if (ramfs_lookup(dir, name) != NULL) return NULL;

    RamfsNode *child = ramfs_alloc_node(type);
    if (child == NULL) return NULL;

    RamfsDirEntry *entry = &parent->children[parent->num_children];
    strncpy(entry->name, name, RAMFS_NAME_MAX);
    entry->name[RAMFS_NAME_MAX] = '\0';
    entry->node = child;
    parent->num_children++;

    return &child->vnode;
}

static int ramfs_unlink(VfsNode *dir, const char *name) {
    RamfsNode *parent = to_ramfs(dir);
    if (dir->type != VFS_DIRECTORY) return -ENOTDIR;

    for (uint32_t i = 0; i < parent->num_children; i++) {
        if (strcmp(parent->children[i].name, name) == 0) {
            RamfsNode *child = parent->children[i].node;

            /* Don't unlink non-empty directories */
            if (child->vnode.type == VFS_DIRECTORY && child->num_children > 0) {
                return -ENOTEMPTY;
            }

            /* Free the child node's resources */
            if (child->data) kfree(child->data);
            if (child->children) kfree(child->children);
            kfree(child);

            /* Compact the children array */
            for (uint32_t j = i; j + 1 < parent->num_children; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->num_children--;

            return VFS_OK;
        }
    }

    return -ENOENT;
}

static int ramfs_readdir(VfsNode *dir, VfsDirEntry *entries, uint32_t max) {
    RamfsNode *rn = to_ramfs(dir);
    if (dir->type != VFS_DIRECTORY) return -ENOTDIR;

    uint32_t count = (rn->num_children < max) ? rn->num_children : max;
    for (uint32_t i = 0; i < count; i++) {
        strncpy(entries[i].name, rn->children[i].name, 255);
        entries[i].name[255] = '\0';
        entries[i].inode_num = rn->children[i].node->vnode.inode_num;
        entries[i].type = rn->children[i].node->vnode.type;
    }

    return (int)count;
}

static void ramfs_truncate(VfsNode *node, uint64_t size) {
    RamfsNode *rn = to_ramfs(node);
    if (node->type != VFS_FILE) return;

    if (size == 0) {
        if (rn->data) {
            kfree(rn->data);
            rn->data = NULL;
        }
        rn->capacity = 0;
        node->size = 0;
    } else if (size < node->size) {
        node->size = size;
    }
}

VfsNode *ramfs_init(void) {
    next_inode = 1;
    RamfsNode *root = ramfs_alloc_node(VFS_DIRECTORY);
    if (root == NULL) return NULL;
    root->vnode.mode = 0755;
    return &root->vnode;
}
