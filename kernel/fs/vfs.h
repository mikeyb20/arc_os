#ifndef ARCHOS_FS_VFS_H
#define ARCHOS_FS_VFS_H

#include <stddef.h>
#include <stdint.h>

/* Node types */
#define VFS_FILE      0
#define VFS_DIRECTORY 1
#define VFS_PIPE      2

/* Maximum length of a path component name (excluding NUL) */
#define VFS_NAME_MAX  256

/* Open flags */
#define O_RDONLY   0x00
#define O_WRONLY   0x01
#define O_RDWR     0x02
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400

/* Access mode mask (low 2 bits) */
#define O_ACCMODE  0x03

/* Seek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Error codes (negative) */
#define VFS_OK       0
#define ENOENT       2
#define EBADF        9
#define ENOMEM      12
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENOSPC      28
#define ENOSYS      38
#define EIO          5
#define ENOTEMPTY   39
#define ECHILD      10
#define EAGAIN      11
#define ESPIPE      29
#define EPIPE       32

/* Forward declarations */
typedef struct VfsNode VfsNode;
typedef struct VfsDirEntry VfsDirEntry;

/* Operations table — each filesystem implements these.
 *
 * Return convention: negative errno on error, 0 or positive on success.
 * All callbacks are optional (may be NULL). The VFS layer checks for NULL
 * before calling and returns -ENOSYS if the operation is not supported.
 *
 * read:     Read up to 'size' bytes at 'offset' into 'buf'. Returns bytes read (>=0).
 * write:    Write up to 'size' bytes from 'buf' at 'offset'. Returns bytes written (>=0).
 * lookup:   Find a child by name in 'dir'. Returns VfsNode* or NULL if not found.
 * create:   Create a new child (file or dir) in 'dir'. Returns VfsNode* or NULL on error.
 * unlink:   Remove a child by name from 'dir'. Returns 0 on success.
 * readdir:  Fill 'entries' with up to 'max' directory entries. Returns entry count (>=0).
 * truncate: Set node size to 'size', discarding data beyond. No return value.
 */
typedef struct {
    int      (*read)(VfsNode *node, void *buf, uint32_t offset, uint32_t size);
    int      (*write)(VfsNode *node, const void *buf, uint32_t offset, uint32_t size);
    VfsNode *(*lookup)(VfsNode *dir, const char *name);
    VfsNode *(*create)(VfsNode *dir, const char *name, uint8_t type);
    int      (*unlink)(VfsNode *dir, const char *name);
    int      (*readdir)(VfsNode *dir, VfsDirEntry *entries, uint32_t max);
    void     (*truncate)(VfsNode *node, uint64_t size);
} VfsOps;

/* VFS Node — inode equivalent */
struct VfsNode {
    uint64_t       inode_num;
    uint8_t        type;          /* VFS_FILE or VFS_DIRECTORY */
    uint64_t       size;
    uint32_t       mode;
    const VfsOps  *ops;
    void          *private_data;  /* fs-specific (RamfsNode* etc.) */
};

/* Open file handle */
typedef struct {
    VfsNode  *node;
    uint64_t  offset;
    uint32_t  flags;
} VfsFile;

/* Directory entry (for readdir) */
struct VfsDirEntry {
    char     name[VFS_NAME_MAX];
    uint64_t inode_num;
    uint8_t  type;
};

/* File stat info */
typedef struct {
    uint64_t inode_num;
    uint8_t  type;
    uint64_t size;
    uint32_t mode;
} VfsStat;

/* Initialize the VFS layer */
void vfs_init(void);

/* Set/get the root filesystem node */
void vfs_set_root(VfsNode *root);
VfsNode *vfs_get_root(void);

/* Resolve an absolute path to a VfsNode */
VfsNode *vfs_resolve(const char *path);

/* File operations */
int vfs_open(const char *path, uint32_t flags, VfsFile *out);
int vfs_close(VfsFile *file);
int vfs_read(VfsFile *file, void *buf, uint32_t size);
int vfs_write(VfsFile *file, const void *buf, uint32_t size);
int vfs_seek(VfsFile *file, int64_t offset, int whence);

/* Metadata operations */
int vfs_stat(const char *path, VfsStat *out);

/* Directory operations */
int vfs_mkdir(const char *path, uint32_t mode);
int vfs_readdir(const char *path, VfsDirEntry *entries, uint32_t max);
int vfs_unlink(const char *path);

#endif /* ARCHOS_FS_VFS_H */
