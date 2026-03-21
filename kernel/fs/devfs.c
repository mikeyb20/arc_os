#include "fs/devfs.h"
#include "drivers/tty.h"
#include "lib/mem.h"
#include "lib/string.h"

/* Device type tags */
#define DEV_NULL  0
#define DEV_ZERO  1
#define DEV_TTY   2

/* Node that wraps VfsNode with a device type */
typedef struct {
    VfsNode  vnode;
    uint8_t  dev_type;
} DevfsNode;

/* Static nodes — no dynamic allocation */
static DevfsNode dev_root_node;
static DevfsNode dev_null_node;
static DevfsNode dev_zero_node;
static DevfsNode dev_tty_node;

/* --- /dev/null ops --- */

static int devfs_null_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size) {
    (void)node; (void)buf; (void)offset; (void)size;
    return 0;  /* EOF */
}

static int devfs_null_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size) {
    (void)node; (void)buf; (void)offset;
    return (int)size;  /* discard */
}

static const VfsOps devfs_null_ops = {
    .read  = devfs_null_read,
    .write = devfs_null_write,
};

/* --- /dev/zero ops --- */

static int devfs_zero_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size) {
    (void)node; (void)offset;
    memset(buf, 0, size);
    return (int)size;
}

static int devfs_zero_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size) {
    (void)node; (void)buf; (void)offset;
    return (int)size;
}

static const VfsOps devfs_zero_ops = {
    .read  = devfs_zero_read,
    .write = devfs_zero_write,
};

/* --- /dev/tty ops --- */

static int devfs_tty_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size) {
    (void)node; (void)offset;
    return tty_read(buf, size);
}

static int devfs_tty_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size) {
    (void)node; (void)offset;
    return tty_write(buf, size);
}

static const VfsOps devfs_tty_ops = {
    .read  = devfs_tty_read,
    .write = devfs_tty_write,
};

/* --- /dev directory ops --- */

static struct {
    const char *name;
    DevfsNode  *node;
} dev_entries[] = {
    { "null", &dev_null_node },
    { "zero", &dev_zero_node },
    { "tty",  &dev_tty_node },
};
#define DEV_ENTRY_COUNT 3

static VfsNode *devfs_dir_lookup(VfsNode *dir, const char *name) {
    (void)dir;
    for (int i = 0; i < DEV_ENTRY_COUNT; i++) {
        if (strcmp(name, dev_entries[i].name) == 0) {
            return &dev_entries[i].node->vnode;
        }
    }
    return NULL;
}

static int devfs_dir_readdir(VfsNode *dir, VfsDirEntry *entries, uint32_t max) {
    (void)dir;
    uint32_t count = DEV_ENTRY_COUNT < max ? DEV_ENTRY_COUNT : max;
    for (uint32_t i = 0; i < count; i++) {
        strncpy(entries[i].name, dev_entries[i].name, VFS_NAME_MAX - 1);
        entries[i].name[VFS_NAME_MAX - 1] = '\0';
        entries[i].inode_num = dev_entries[i].node->vnode.inode_num;
        entries[i].type = dev_entries[i].node->vnode.type;
    }
    return (int)count;
}

static const VfsOps devfs_dir_ops = {
    .lookup  = devfs_dir_lookup,
    .readdir = devfs_dir_readdir,
};

/* --- Init --- */

static void devfs_init_node(DevfsNode *dn, uint64_t ino, uint8_t type,
                            uint32_t mode, const VfsOps *ops, uint8_t dev_type) {
    dn->vnode.inode_num = ino;
    dn->vnode.type = type;
    dn->vnode.size = 0;
    dn->vnode.mode = mode;
    dn->vnode.ops = ops;
    dn->vnode.private_data = dn;
    dn->dev_type = dev_type;
}

VfsNode *devfs_init(void) {
    devfs_init_node(&dev_root_node, 1000, VFS_DIRECTORY, 0755, &devfs_dir_ops, 0);
    devfs_init_node(&dev_null_node, 1001, VFS_FILE, 0666, &devfs_null_ops, DEV_NULL);
    devfs_init_node(&dev_zero_node, 1002, VFS_FILE, 0666, &devfs_zero_ops, DEV_ZERO);
    devfs_init_node(&dev_tty_node,  1003, VFS_FILE, 0666, &devfs_tty_ops,  DEV_TTY);
    return &dev_root_node.vnode;
}
