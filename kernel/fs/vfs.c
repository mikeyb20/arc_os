/* arc_os — mounts and names: path walk, mount table, namespace operations
 * (vfs.h, M1-11)
 *
 * Owns: the mount table, the one path walk every path goes through, and the
 * operations that change directories (create, mkdir, unlink, rmdir, rename)
 * and attributes (setattr), with their permission checks.
 *
 * Paths are absolute and normalized (path_normalize): no ".", "..", or
 * repeated slashes. ".." is therefore resolved on the path text, which is
 * exact while no filesystem has symlinks (see M1-11 Notes).
 *
 * Mounts: entry 0 is "/". Every other entry places a filesystem root, or any
 * directory (a bind-style subtree mount, ADR-0009), over an existing
 * directory, which gets I_MOUNTPOINT. Entries are added under mount_lock and
 * published by a release store of nr_mounts; they are never removed (no
 * umount yet), so readers scan [0, nr_mounts) without the lock. A walk looks
 * at the table only when a component has I_MOUNTPOINT.
 *
 * Locks (docs/kernel/locking.md): a walk holds no lock across components
 * (each lookup takes its directory shared, or none on a dcache hit). Changes
 * take the parent exclusive (LOCK_LEVEL_INODE); rmdir then the victim
 * (LOCK_LEVEL_INODE_CHILD). Rename takes rename_mutex *before* it resolves
 * either path, then the two parents ancestor first (LOCK_LEVEL_INODE,
 * LOCK_LEVEL_INODE_2), then a replaced directory (LOCK_LEVEL_INODE_CHILD).
 * Only rename moves a directory, so while rename_mutex is held the tree's
 * shape can't change under the resolved paths: their text gives the exact
 * ancestry (one mount, no hard links to directories), which orders the two
 * parent locks and rejects moving a directory into itself. Every path that
 * holds two inode locks takes an ancestor before its descendant, so they
 * can't cycle.
 *
 * Cost: a walk is O(components): a dcache probe each, a filesystem lookup on
 * a miss, a scan of at most VFS_MAX_MOUNTS entries at a mount point. */

#include "fs/vfs_internal.h"
#include "fs/path.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "proc/process.h"

#define VFS_MAX_MOUNTS   16
#define MOUNT_PATH_MAX   128
#define MOUNT_SOURCE_MAX 32

struct Mount {
    Inode *root;       /* what the mount shows (a reference) */
    Inode *covered;    /* the directory it hides (a reference); NULL for "/" */
    SuperBlock *sb;
    char path[MOUNT_PATH_MAX];
    char source[MOUNT_SOURCE_MAX];
};

static Mount mounts[VFS_MAX_MOUNTS];
static uint32_t nr_mounts;
static Spinlock mount_lock = SPINLOCK_INIT(LOCK_LEVEL_MOUNT, "mount table");
/* One rename at a time, held across resolving both paths (see above) */
static Mutex rename_mutex = MUTEX_INIT(LOCK_LEVEL_RENAME);

/* --- Credentials of the caller (the kernel itself is root) --- */

typedef struct {
    uint32_t uid;
    uint32_t gid;
    uint32_t umask;
} Cred;

static Cred current_cred(void) {
    Process *p = proc_current();
    Cred c = {0, 0, 0};
    if (p != NULL) {
        c.uid = p->euid;
        c.gid = p->egid;
        c.umask = p->umask;
    }
    return c;
}

/* --- Mount table --- */

static uint32_t mounts_published(void) {
    return __atomic_load_n(&nr_mounts, __ATOMIC_ACQUIRE);
}

/* The mount over dir (the most recent), or NULL */
static Mount *mount_covering(const Inode *dir) {
    for (uint32_t i = mounts_published(); i-- > 1;) {
        if (mounts[i].covered == dir) return &mounts[i];
    }
    return NULL;
}

/* Step from a mount point to what is mounted on it, as often as needed. */
static void cross_mounts(PathLoc *loc) {
    while (__atomic_load_n(&loc->inode->flags, __ATOMIC_ACQUIRE) & I_MOUNTPOINT) {
        Mount *m = mount_covering(loc->inode);
        if (m == NULL) return;
        Inode *root = igrab(m->root);
        iput(loc->inode);
        loc->inode = root;
        loc->mnt = m;
    }
}

static int mount_add(const char *path, const char *source, PathLoc *target, Inode *root, SuperBlock *sb) {
    if (strlen(path) >= MOUNT_PATH_MAX) return -ENAMETOOLONG;
    if (!S_ISDIR(target->inode->mode) || !S_ISDIR(root->mode)) return -ENOTDIR;
    uint64_t flags;
    spin_lock_irqsave(&mount_lock, &flags);
    uint32_t n = nr_mounts;
    int err = 0;
    /* One mount per directory: a covered one, or the root of a mount, is busy */
    if ((target->inode->flags & I_MOUNTPOINT) || target->inode == target->mnt->root)
        err = -EBUSY;
    else if (n >= VFS_MAX_MOUNTS)
        err = -ENOMEM;
    if (err == 0) {
        Mount *m = &mounts[n];
        m->root = igrab(root);
        m->covered = igrab(target->inode);
        m->sb = sb;
        strncpy(m->path, path, MOUNT_PATH_MAX - 1);
        m->path[MOUNT_PATH_MAX - 1] = '\0';
        strncpy(m->source, source != NULL ? source : "none", MOUNT_SOURCE_MAX - 1);
        m->source[MOUNT_SOURCE_MAX - 1] = '\0';
        __atomic_store_n(&nr_mounts, n + 1, __ATOMIC_RELEASE);   /* entry first, then the flag */
        __atomic_or_fetch(&target->inode->flags, I_MOUNTPOINT, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&mount_lock, flags);
    return err;
}

int vfs_mount_root(SuperBlock *sb, const char *source) {
    if (sb == NULL || sb->root == NULL) return -EINVAL;
    if (mounts_published() != 0) return -EBUSY;
    Mount *m = &mounts[0];
    m->root = igrab(sb->root);
    m->covered = NULL;
    m->sb = sb;
    strncpy(m->path, "/", MOUNT_PATH_MAX - 1);
    strncpy(m->source, source != NULL ? source : "none", MOUNT_SOURCE_MAX - 1);
    __atomic_store_n(&nr_mounts, 1, __ATOMIC_RELEASE);
    return 0;
}

int vfs_mount(const char *path, SuperBlock *sb, const char *source) {
    if (sb == NULL || sb->root == NULL) return -EINVAL;
    PathLoc target;
    int err = vfs_resolve(path, &target);
    if (err != 0) return err;
    err = mount_add(path, source, &target, sb->root, sb);
    loc_put(&target);
    return err;
}

int vfs_bind_mount(const char *src, const char *target) {
    PathLoc from, to;
    int err = vfs_resolve(src, &from);
    if (err != 0) return err;
    err = vfs_resolve(target, &to);
    if (err == 0) {
        err = mount_add(target, src, &to, from.inode, from.inode->sb);
        loc_put(&to);
    }
    loc_put(&from);
    return err;
}

static int mounts_append(char *buf, int pos, int size, const char *s) {
    while (*s != '\0' && pos < size - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
    return pos;
}

int vfs_format_mounts(char *buf, int size) {
    if (size <= 0) return 0;
    int pos = 0;
    buf[0] = '\0';
    uint32_t n = mounts_published();
    for (uint32_t i = 0; i < n; i++) {
        pos = mounts_append(buf, pos, size, mounts[i].source);
        pos = mounts_append(buf, pos, size, " ");
        pos = mounts_append(buf, pos, size, mounts[i].path);
        pos = mounts_append(buf, pos, size, " ");
        pos = mounts_append(buf, pos, size, mounts[i].sb->type);
        pos = mounts_append(buf, pos, size, (mounts[i].sb->flags & SB_RDONLY) ? " ro 0 0\n" : " rw 0 0\n");
    }
    return pos;
}

/* --- Lookup --- */

/* Find name in dir, which the caller has locked (shared or exclusive).
 * 0 with *out (a reference), or -ENOENT. */
static int lookup_locked(Inode *dir, const char *name, Inode **out) {
    if (dir->flags & I_DEAD) return -ENOENT;
    int cacheable = !(dir->sb->flags & SB_NO_DCACHE);
    if (cacheable) {
        *out = dcache_lookup(dir, name);
        if (*out != NULL) return 0;
    }
    if (dir->iop == NULL || dir->iop->lookup == NULL) return -ENOTDIR;
    int err = dir->iop->lookup(dir, name, out);
    if (err == 0 && cacheable) dcache_add(dir, name, *out);
    return err;
}

/* Find name in dir (not locked): the dcache, else the filesystem under the
 * directory's shared lock. */
static int lookup_one(Inode *dir, const char *name, Inode **out) {
    if (!(dir->sb->flags & SB_NO_DCACHE)) {
        *out = dcache_lookup(dir, name);
        if (*out != NULL) return 0;
    }
    rwsem_down_read(&dir->lock);
    int err = lookup_locked(dir, name, out);
    rwsem_up_read(&dir->lock);
    return err;
}

/* Copy the component at *p into comp and step past it. Returns its length,
 * 0 at the end of the path, or -ENAMETOOLONG. */
static int next_component(const char **p, char comp[VFS_NAME_MAX + 1]) {
    const char *s = *p;
    while (*s == '/') s++;
    size_t n = 0;
    while (s[n] != '\0' && s[n] != '/') n++;
    if (n > VFS_NAME_MAX) return -ENAMETOOLONG;
    memcpy(comp, s, n);
    comp[n] = '\0';
    *p = s + n;
    return (int)n;
}

/* Is the rest of the path (from p) only slashes? */
static int at_end(const char *p) {
    while (*p == '/') p++;
    return *p == '\0';
}

/* Walk path from "/". With last != NULL, stop before the final component and
 * copy it there (-EINVAL if the path has none). */
static int walk_path(const char *path, PathLoc *out, char *last) {
    if (path == NULL || path[0] != '/') return -EINVAL;
    uint32_t n = mounts_published();
    if (n == 0) return -ENOENT;
    PathLoc loc = {&mounts[0], igrab(mounts[0].root)};
    char comp[VFS_NAME_MAX + 1];
    const char *p = path;
    int err = 0;
    for (;;) {
        int len = next_component(&p, comp);
        if (len < 0) {
            err = len;
            break;
        }
        if (len == 0) {
            if (last != NULL) err = -EINVAL;   /* "/" has no final component */
            break;
        }
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            err = -EINVAL;   /* not normalized */
            break;
        }
        if (!S_ISDIR(loc.inode->mode)) {
            err = -ENOTDIR;
            break;
        }
        if (last != NULL && at_end(p)) {
            memcpy(last, comp, (size_t)len + 1);
            break;
        }
        if (len == 1 && comp[0] == '.') continue;
        err = inode_permission(loc.inode, X_OK);
        if (err != 0) break;
        Inode *child;
        err = lookup_one(loc.inode, comp, &child);
        if (err != 0) break;
        iput(loc.inode);
        loc.inode = child;
        cross_mounts(&loc);
    }
    if (err != 0) {
        iput(loc.inode);
        return err;
    }
    *out = loc;
    return 0;
}

int vfs_resolve(const char *path, PathLoc *out) {
    return walk_path(path, out, NULL);
}

int vfs_resolve_parent(const char *path, PathLoc *dir, char name[VFS_NAME_MAX + 1]) {
    int err = walk_path(path, dir, name);
    if (err == 0 && !S_ISDIR(dir->inode->mode)) {
        loc_put(dir);
        return -ENOTDIR;
    }
    return err;
}

void loc_put(PathLoc *loc) {
    if (loc->inode != NULL) iput(loc->inode);
    loc->inode = NULL;
}

/* --- Changing directories --- */

/* May the caller add or remove entries in dir? */
static int may_change_dir(Inode *dir) {
    if (dir->sb->flags & SB_RDONLY) return -EROFS;
    return inode_permission(dir, W_OK | X_OK);
}

/* Sticky directories (/tmp): only the owner of the entry or of the
 * directory, or root, may remove or rename it. */
static int may_delete(const Inode *dir, const Inode *victim, const Cred *c) {
    if (!(dir->mode & S_ISVTX) || c->uid == 0) return 0;
    if (c->uid == victim->uid || c->uid == dir->uid) return 0;
    return -EPERM;
}

static void touch_dir(Inode *dir) {
    vfs_now(&dir->mtime);
    dir->ctime = dir->mtime;
}

int vfs_create_locked(Inode *dir, const char *name, uint32_t mode, Inode **out) {
    if (dir->flags & I_DEAD) return -ENOENT;
    int err = may_change_dir(dir);
    if (err != 0) return err;
    if (dir->iop == NULL || dir->iop->create == NULL) return -EACCES;   /* Linux: no create op is EACCES */
    Cred c = current_cred();
    err = dir->iop->create(dir, name, S_IFREG | (mode & 07777 & ~c.umask), c.uid, c.gid, out);
    if (err == 0) touch_dir(dir);
    return err;
}

int vfs_open_locate(const char *path, uint32_t flags, uint32_t mode, PathLoc *out, int *created) {
    *created = 0;
    if (!(flags & O_CREAT)) return vfs_resolve(path, out);

    char name[VFS_NAME_MAX + 1];
    PathLoc dir;
    int err = vfs_resolve_parent(path, &dir, name);
    if (err == -EINVAL) return -EISDIR;   /* "/" */
    if (err != 0) return err;
    err = inode_permission(dir.inode, X_OK);
    if (err != 0) {
        loc_put(&dir);
        return err;
    }
    Inode *inode = NULL;
    rwsem_down_write(&dir.inode->lock);
    err = lookup_locked(dir.inode, name, &inode);
    if (err == 0 && (flags & O_EXCL)) {
        iput(inode);
        err = -EEXIST;
    } else if (err == -ENOENT) {
        err = vfs_create_locked(dir.inode, name, mode, &inode);
        *created = (err == 0);
    }
    rwsem_up_write(&dir.inode->lock);
    if (err == 0) {
        out->mnt = dir.mnt;
        out->inode = inode;
        cross_mounts(out);
    }
    loc_put(&dir);
    return err;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    char name[VFS_NAME_MAX + 1];
    PathLoc dir;
    int err = vfs_resolve_parent(path, &dir, name);
    if (err == -EINVAL) err = -EEXIST;   /* "/" */
    if (err != 0) return err;
    Inode *d = dir.inode;
    rwsem_down_write(&d->lock);
    Inode *existing;
    err = lookup_locked(d, name, &existing);
    if (err == 0) {
        iput(existing);
        err = -EEXIST;
    } else if (err == -ENOENT) {
        /* lookup_locked says -ENOENT for a removed directory too: never
         * create in one (its entries would be lost with it) */
        err = (d->flags & I_DEAD) ? -ENOENT : may_change_dir(d);
        if (err == 0 && (d->iop == NULL || d->iop->mkdir == NULL)) err = -EPERM;
        if (err == 0) {
            Cred c = current_cred();
            err = d->iop->mkdir(d, name, S_IFDIR | (mode & 07777 & ~c.umask), c.uid, c.gid);
        }
        if (err == 0) touch_dir(d);
    }
    rwsem_up_write(&d->lock);
    loc_put(&dir);
    return err;
}

int vfs_unlink(const char *path) {
    char name[VFS_NAME_MAX + 1];
    PathLoc dir;
    int err = vfs_resolve_parent(path, &dir, name);
    if (err == -EINVAL) err = -EISDIR;   /* "/" */
    if (err != 0) return err;
    Inode *d = dir.inode;
    Cred c = current_cred();
    Inode *victim = NULL;
    rwsem_down_write(&d->lock);
    err = lookup_locked(d, name, &victim);
    if (err == 0 && S_ISDIR(victim->mode)) err = -EISDIR;
    if (err == 0) err = may_change_dir(d);
    if (err == 0) err = may_delete(d, victim, &c);
    if (err == 0 && (d->iop == NULL || d->iop->unlink == NULL)) err = -EPERM;
    if (err == 0) {
        dcache_remove(d, name);
        err = d->iop->unlink(d, name, victim);
        if (err == 0) {
            touch_dir(d);
            victim->ctime = d->mtime;
        }
    }
    rwsem_up_write(&d->lock);
    if (victim != NULL) iput(victim);
    loc_put(&dir);
    return err;
}

/* Is inode the root of any mount? (It can't be removed or replaced.) */
static int is_mount_root(const Inode *inode) {
    uint32_t n = mounts_published();
    for (uint32_t i = 0; i < n; i++) {
        if (mounts[i].root == inode) return 1;
    }
    return 0;
}

static int busy(const Inode *inode) {
    return (inode->flags & I_MOUNTPOINT) || (S_ISDIR(inode->mode) && is_mount_root(inode));
}

int vfs_rmdir(const char *path) {
    char name[VFS_NAME_MAX + 1];
    PathLoc dir;
    int err = vfs_resolve_parent(path, &dir, name);
    if (err == -EINVAL) err = -EBUSY;   /* "/" */
    if (err != 0) return err;
    Inode *d = dir.inode;
    Cred c = current_cred();
    Inode *victim = NULL;
    rwsem_down_write(&d->lock);
    err = lookup_locked(d, name, &victim);
    if (err == 0 && !S_ISDIR(victim->mode)) err = -ENOTDIR;
    if (err == 0 && busy(victim)) err = -EBUSY;
    if (err == 0) err = may_change_dir(d);
    if (err == 0) err = may_delete(d, victim, &c);
    if (err == 0 && (d->iop == NULL || d->iop->rmdir == NULL)) err = -EPERM;
    if (err == 0) {
        rwsem_down_write_nested(&victim->lock, LOCK_LEVEL_INODE_CHILD);
        dcache_remove(d, name);
        err = d->iop->rmdir(d, name, victim);   /* -ENOTEMPTY unless empty */
        if (err == 0) {
            __atomic_or_fetch(&victim->flags, I_DEAD, __ATOMIC_RELAXED);
            touch_dir(d);
        }
        rwsem_up_write(&victim->lock);
    }
    rwsem_up_write(&d->lock);
    if (victim != NULL) iput(victim);
    loc_put(&dir);
    return err;
}

/* Is a (a normalized path) an ancestor of b, or b itself when or_self? */
static int path_is_ancestor(const char *a, const char *b, int or_self) {
    size_t n = strlen(a);
    if (strncmp(a, b, n) != 0) return 0;
    if (b[n] == '\0') return or_self;
    return n == 1 || b[n] == '/';   /* a is "/" or a whole leading component */
}

/* Parent part of a normalized path ("/a/b" -> "/a", "/a" -> "/") into out. */
static void parent_path(const char *path, char out[PATH_MAX]) {
    strncpy(out, path, PATH_MAX - 1);
    out[PATH_MAX - 1] = '\0';
    char *slash = out;
    for (char *s = out; *s != '\0'; s++) {
        if (*s == '/') slash = s;
    }
    if (slash == out) slash++;   /* keep the root's "/" */
    *slash = '\0';
}

/* Checks on a rename's source and replaced entry, both directories locked. */
static int rename_check(Inode *odir, Inode *src, Inode *ndir, Inode *victim, const Cred *c) {
    if (ndir->flags & I_DEAD) return -ENOENT;
    int err = may_change_dir(odir);
    if (err == 0 && ndir != odir) err = may_change_dir(ndir);
    if (err == 0) err = may_delete(odir, src, c);
    if (err == 0 && busy(src)) err = -EBUSY;
    if (err != 0 || victim == NULL) return err;
    if (S_ISDIR(src->mode) && !S_ISDIR(victim->mode)) return -ENOTDIR;
    if (!S_ISDIR(src->mode) && S_ISDIR(victim->mode)) return -EISDIR;
    if (busy(victim)) return -EBUSY;
    return may_delete(ndir, victim, c);
}

/* Lock the two parents of a rename, ancestor first. */
static void rename_lock_dirs(Inode *odir, const char *opath, Inode *ndir, const char *npath) {
    if (odir == ndir) {
        rwsem_down_write(&odir->lock);
        return;
    }
    char op[PATH_MAX], np[PATH_MAX];
    parent_path(opath, op);
    parent_path(npath, np);
    Inode *first = path_is_ancestor(np, op, 0) ? ndir : odir;
    rwsem_down_write(&first->lock);
    rwsem_down_write_nested(&(first == odir ? ndir : odir)->lock, LOCK_LEVEL_INODE_2);
}

/* The rename itself, the parents resolved and in the same mount. */
static int rename_in(PathLoc *od, const char *oname, PathLoc *nd, const char *nname, const char *oldpath,
                     const char *newpath) {
    Inode *odir = od->inode, *ndir = nd->inode;
    Cred c = current_cred();
    Inode *src = NULL, *victim = NULL;
    rename_lock_dirs(odir, oldpath, ndir, newpath);
    int err = lookup_locked(odir, oname, &src);
    if (err == 0) {
        err = lookup_locked(ndir, nname, &victim);
        if (err == -ENOENT) err = 0;
    }
    int locked_victim = 0;
    if (err == 0 && src != victim) {
        /* A target that contains the source (rename("/a/b/c", "/a/b")) is
         * not empty; it is one of the parents we hold, so decide on the path
         * text instead of locking it again */
        if (path_is_ancestor(newpath, oldpath, 0)) err = -ENOTEMPTY;
        if (err == 0 && S_ISDIR(src->mode) && path_is_ancestor(oldpath, newpath, 0)) err = -EINVAL; /* into itself */
        if (err == 0) err = rename_check(odir, src, ndir, victim, &c);
        if (err == 0 && (odir->iop == NULL || odir->iop->rename == NULL)) err = -EPERM;
        if (err == 0 && victim != NULL && S_ISDIR(victim->mode)) {
            rwsem_down_write_nested(&victim->lock, LOCK_LEVEL_INODE_CHILD);
            locked_victim = 1;
        }
        if (err == 0) {
            dcache_remove(odir, oname);
            dcache_remove(ndir, nname);
            err = odir->iop->rename(odir, oname, src, ndir, nname, victim);
        }
        if (err == 0) {
            if (locked_victim) __atomic_or_fetch(&victim->flags, I_DEAD, __ATOMIC_RELAXED);
            touch_dir(odir);
            if (ndir != odir) touch_dir(ndir);
            src->ctime = odir->mtime;
        }
    }
    if (locked_victim) rwsem_up_write(&victim->lock);
    if (ndir != odir) rwsem_up_write(&ndir->lock);
    rwsem_up_write(&odir->lock);
    if (src != NULL) iput(src);
    if (victim != NULL) iput(victim);
    return err;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    char oname[VFS_NAME_MAX + 1], nname[VFS_NAME_MAX + 1];
    PathLoc od, nd;
    mutex_lock(&rename_mutex);   /* before resolving: no directory moves until we're done */
    int err = vfs_resolve_parent(oldpath, &od, oname);
    if (err == -EINVAL) err = -EBUSY; /* "/" */
    if (err != 0) {
        mutex_unlock(&rename_mutex);
        return err;
    }
    err = vfs_resolve_parent(newpath, &nd, nname);
    if (err == -EINVAL) err = -EBUSY;
    if (err == 0) {
        err = od.mnt != nd.mnt ? -EXDEV : rename_in(&od, oname, &nd, nname, oldpath, newpath);
        loc_put(&nd);
    }
    loc_put(&od);
    mutex_unlock(&rename_mutex);
    return err;
}

int vfs_stat(const char *path, struct stat *st) {
    PathLoc loc;
    int err = vfs_resolve(path, &loc);
    if (err != 0) return err;
    vfs_fill_stat(loc.inode, st);
    loc_put(&loc);
    return 0;
}

int vfs_setattr(Inode *inode, const InodeAttr *attr) {
    if (inode->sb->flags & SB_RDONLY) return -EROFS;
    Cred c = current_cred();
    if ((attr->valid & ATTR_MODE) && c.uid != 0 && c.uid != inode->uid) return -EPERM;
    if ((attr->valid & (ATTR_UID | ATTR_GID)) && c.uid != 0) return -EPERM;
    if (attr->valid & ATTR_SIZE) {
        if (S_ISDIR(inode->mode)) return -EISDIR;
        if (!S_ISREG(inode->mode)) return -EINVAL;
        if (attr->size < 0) return -EINVAL;
    }
    rwsem_down_write(&inode->lock);
    int err = 0;
    if (inode->iop != NULL && inode->iop->setattr != NULL) {
        err = inode->iop->setattr(inode, attr);
    } else if (attr->valid & ATTR_SIZE) {
        err = -EPERM;
    } else {
        if (attr->valid & ATTR_MODE) inode->mode = (inode->mode & S_IFMT) | (attr->mode & 07777);
        if (attr->valid & ATTR_UID) inode->uid = attr->uid;
        if (attr->valid & ATTR_GID) inode->gid = attr->gid;
    }
    if (err == 0) vfs_now(&inode->ctime);
    rwsem_up_write(&inode->lock);
    return err;
}
