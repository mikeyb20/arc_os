/* arc_os — mounts and names: path walk, mount table, namespace operations
 * (vfs.h, M1-11; the walk rework is M1-13)
 *
 * Owns: the mount table, the one path walk every path goes through, the
 * working directory of a process, and the operations that change directories
 * (create, mkdir, unlink, rmdir, rename) and attributes (setattr), with their
 * permission checks.
 *
 * The walk takes a path as the caller gave it and a starting point (PathLoc:
 * a mount and an inode; the cwd or a dirfd's directory, "/" for absolute
 * paths). "." stays, ".." steps to the parent the filesystem names (its ".."
 * entry), and at the root of a mount first back to the directory the mount
 * covers; at "/" it stays. A symlink met before the last component, or as the
 * last one when the caller follows (or a '/' follows it), is replaced by its
 * target: the rest of the path is appended to the target, and an absolute
 * target restarts at "/". At most VFS_MAX_LINKS links per walk (-ELOOP).
 * Nothing is resolved on the path text, so "a/link/.." is the link target's
 * parent, as on Linux, and a cwd or dirfd keeps working when its directory is
 * renamed.
 *
 * Mounts: entry 0 is "/". Every other entry places a filesystem root, or any
 * directory (a bind-style subtree mount, ADR-0009), over an existing
 * directory, which gets I_MOUNTPOINT. Entries are added under mount_lock and
 * published by a release store of nr_mounts; they are never removed (no
 * umount yet), so readers scan [0, nr_mounts) without the lock. A walk looks
 * at the table only when a component has I_MOUNTPOINT.
 *
 * Locks (docs/kernel/locking.md): a walk holds no lock across components
 * (each lookup takes its directory shared, or none on a dcache hit; ".."
 * takes it shared too, never cached). Changes take the parent exclusive
 * (LOCK_LEVEL_INODE); rmdir then the victim (LOCK_LEVEL_INODE_CHILD). Rename
 * takes rename_mutex *before* it resolves either path, then works out
 * whether one parent is an ancestor of the other by walking ".." up from
 * each (only rename moves a directory, so the answer holds until it is done),
 * then locks the two parents ancestor first (LOCK_LEVEL_INODE,
 * LOCK_LEVEL_INODE_2), a moved directory (LOCK_LEVEL_INODE_CHILD: its ".."
 * changes) and a replaced directory (LOCK_LEVEL_INODE_CHILD_2). Every path
 * that holds two inode locks takes an ancestor before its descendant, so they
 * can't cycle.
 *
 * Cost: a walk is O(components + links followed): a dcache probe each, a
 * filesystem lookup on a miss or for "..", a scan of at most VFS_MAX_MOUNTS
 * entries at a mount point. Symlink targets go through one 1 KiB buffer
 * allocated only when a walk meets a link. Rename adds O(depth) ".." lookups;
 * vfs_path_of (getcwd) O(depth x entries). */

#include "fs/vfs_internal.h"
#include "fs/path.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "mm/kmalloc.h"
#include "proc/process.h"

#define VFS_MAX_MOUNTS   16
#define MOUNT_PATH_MAX   128
#define MOUNT_SOURCE_MAX 32

struct Mount {
    Inode *root;       /* what the mount shows (a reference) */
    Inode *covered;    /* the directory it hides (a reference); NULL for "/" */
    Mount *parent;     /* the mount `covered` is in; NULL for "/" */
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
        m->parent = target->mnt;
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
    m->parent = NULL;
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

/* "." or ".." */
static int is_dots(const char *name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/* Find name (never "." or "..") in dir, which the caller has locked (shared
 * or exclusive). 0 with *out (a reference), or -ENOENT. */
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

/* dir's parent inside its filesystem (its ".." entry), never cached: a
 * rename changes it. 0 with *out (a reference), -ENOENT for a removed
 * directory. */
static int lookup_dotdot(Inode *dir, Inode **out) {
    rwsem_down_read(&dir->lock);
    int err;
    if (dir->flags & I_DEAD)
        err = -ENOENT;
    else if (dir->iop == NULL || dir->iop->lookup == NULL)
        err = -ENOTDIR;
    else
        err = dir->iop->lookup(dir, "..", out);
    rwsem_up_read(&dir->lock);
    return err;
}

int vfs_mount_subdir(const char *target, SuperBlock *sb, const char *name, const char *source) {
    if (sb == NULL || sb->root == NULL || is_dots(name)) return -EINVAL;
    Inode *dir;
    int err = lookup_one(sb->root, name, &dir);
    if (err != 0) return err;
    PathLoc to;
    err = vfs_resolve(target, &to);
    if (err == 0) {
        err = mount_add(target, source, &to, dir, sb);
        loc_put(&to);
    }
    iput(dir);
    return err;
}

/* --- The walk --- */

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

static void loc_root(PathLoc *loc) {
    loc->mnt = &mounts[0];
    loc->inode = igrab(mounts[0].root);
}

/* Step loc to its parent (".."). At the root of a mount, first back to the
 * directory the mount covers (as often as needed); at "/" it stays. A parent
 * that is itself a mount point shows what is mounted there, as on Linux.
 * A ".." from a corrupt disk is -EIO when it names a non-directory, or the
 * volume's root from below a subtree mount's root (the walk stops at the
 * mount's root first, so no sound walk gets there). */
static int step_up(PathLoc *loc) {
    while (loc->inode == loc->mnt->root) {
        if (loc->mnt->parent == NULL) return 0;   /* "/" */
        Inode *covered = igrab(loc->mnt->covered);
        iput(loc->inode);
        loc->inode = covered;
        loc->mnt = loc->mnt->parent;
    }
    Inode *parent;
    int err = lookup_dotdot(loc->inode, &parent);
    if (err != 0) return err;
    if (!S_ISDIR(parent->mode) || (parent == parent->sb->root && parent != loc->mnt->root)) {
        iput(parent);
        return -EIO;
    }
    iput(loc->inode);
    loc->inode = parent;
    cross_mounts(loc);
    return 0;
}

typedef struct Walk {
    PathLoc loc;
    char *buf;         /* 2 x PATH_MAX for symlink targets, allocated at the first link */
    int cur;           /* the half of buf the path is in now */
    uint32_t links;    /* followed so far */
} Walk;

/* Read link's target into buf (PATH_MAX bytes) with tail appended (the rest
 * of the path being walked, and buf must not hold it). Length, or -errno. */
static int link_target(Inode *link, char *buf, const char *tail) {
    if (link->iop == NULL || link->iop->readlink == NULL) return -EIO;
    int n = link->iop->readlink(link, buf, PATH_MAX);
    if (n < 0) return n;
    if (n == 0) return -ENOENT;   /* an empty target names nothing (Linux) */
    size_t rest = strlen(tail);
    if ((size_t)n + rest >= PATH_MAX) return -ENAMETOOLONG;
    memcpy(buf + n, tail, rest + 1);
    return n;
}

/* Replace the symlink just met by its target: the walk goes on with the
 * target followed by the rest of the path (*p); an absolute target restarts
 * at "/". */
static int walk_follow(Walk *w, Inode *link, const char **p) {
    if (++w->links > VFS_MAX_LINKS) return -ELOOP;
    if (w->buf == NULL) {
        w->buf = kmalloc(2 * PATH_MAX, 0);
        if (w->buf == NULL) return -ENOMEM;
    }
    char *dst = w->buf + (w->cur ^ 1) * PATH_MAX;   /* *p may be in the other half */
    int n = link_target(link, dst, *p);
    if (n < 0) return n;
    w->cur ^= 1;
    *p = dst;
    if (dst[0] == '/') {
        iput(w->loc.inode);
        loc_root(&w->loc);
    }
    return 0;
}

/* Walk path from w->loc. With last != NULL, stop before the last component
 * and copy it there (-EINVAL if the path has none: "/"), *slash saying
 * whether a '/' followed it. */
static int walk_run(Walk *w, const char *path, uint32_t flags, char *last, int *slash) {
    char comp[VFS_NAME_MAX + 1];
    const char *p = path;
    for (;;) {
        int len = next_component(&p, comp);
        if (len < 0) return len;
        if (len == 0) return last != NULL ? -EINVAL : 0;
        if (!S_ISDIR(w->loc.inode->mode)) return -ENOTDIR;
        int final = at_end(p);
        int trailing = final && *p == '/';
        if (final && last != NULL) {
            memcpy(last, comp, (size_t)len + 1);
            *slash = trailing;
            return 0;
        }
        int err = inode_permission(w->loc.inode, X_OK);
        if (err != 0) return err;
        if (len == 1 && comp[0] == '.') continue;
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            err = step_up(&w->loc);
            if (err != 0) return err;
            continue;
        }
        Inode *child;
        err = lookup_one(w->loc.inode, comp, &child);
        if (err != 0) return err;
        if (S_ISLNK(child->mode) && (!final || trailing || (flags & WALK_FOLLOW))) {
            err = walk_follow(w, child, &p);
            iput(child);
            if (err != 0) return err;
            continue;
        }
        iput(w->loc.inode);
        w->loc.inode = child;
        cross_mounts(&w->loc);
        if (final) return trailing && !S_ISDIR(w->loc.inode->mode) ? -ENOTDIR : 0;
    }
}

static int path_walk(const PathLoc *base, const char *path, uint32_t flags, PathLoc *out, char *last, int *slash) {
    if (path == NULL) return -EINVAL;
    if (path[0] == '\0') return -ENOENT;
    if (mounts_published() == 0) return -ENOENT;
    Walk w = {.buf = NULL, .cur = 0, .links = 0};
    if (path[0] != '/' && base != NULL && base->inode != NULL)
        loc_get(&w.loc, base);
    else
        loc_root(&w.loc);
    int err = walk_run(&w, path, flags, last, slash);
    kfree(w.buf);
    if (err != 0) {
        iput(w.loc.inode);
        return err;
    }
    *out = w.loc;
    return 0;
}

int vfs_resolve_at(const PathLoc *base, const char *path, uint32_t flags, PathLoc *out) {
    return path_walk(base, path, flags, out, NULL, NULL);
}

int vfs_resolve_parent_at(const PathLoc *base, const char *path, PathLoc *dir, char name[VFS_NAME_MAX + 1],
                          int *slash) {
    return path_walk(base, path, 0, dir, name, slash);
}

void loc_get(PathLoc *dst, const PathLoc *src) {
    dst->mnt = src->mnt;
    dst->inode = src->inode != NULL ? igrab(src->inode) : NULL;
}

void loc_put(PathLoc *loc) {
    if (loc->inode != NULL) iput(loc->inode);
    loc->inode = NULL;
}

/* --- Working directory (see vfs.h) --- */

void vfs_cwd_get(PathLoc *out) {
    Process *p = proc_current();
    if (p != NULL && p->cwd != NULL) {
        out->mnt = p->cwd_mnt;
        out->inode = igrab(p->cwd);
    } else if (mounts_published() != 0) {
        loc_root(out);
    } else {
        out->mnt = NULL;
        out->inode = NULL;
    }
}

void vfs_cwd_set(PathLoc *loc) {
    Process *p = proc_current();
    if (p == NULL) {
        loc_put(loc);
        return;
    }
    Inode *old = p->cwd;
    p->cwd = loc->inode;
    p->cwd_mnt = loc->mnt;
    loc->inode = NULL;
    if (old != NULL) iput(old);
}

void vfs_cwd_copy(Process *child, const Process *parent) {
    child->cwd = parent->cwd != NULL ? igrab(parent->cwd) : NULL;
    child->cwd_mnt = parent->cwd_mnt;
}

void vfs_cwd_release(Process *p) {
    Inode *cwd = p->cwd;
    p->cwd = NULL;
    p->cwd_mnt = NULL;
    if (cwd != NULL) iput(cwd);
}

/* --- Names of directories (getcwd) --- */

typedef struct {
    DirContext ctx;   /* first: the actor gets &ctx */
    uint64_t ino;
    char *name;
    int found;
} FindIno;

static int find_ino_actor(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    (void)dtype, (void)next;
    FindIno *f = (FindIno *)ctx;
    if (ino != f->ino || len > VFS_NAME_MAX) return 0;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return 0;
    memcpy(f->name, name, len);
    f->name[len] = '\0';
    f->found = 1;
    return 1;   /* stop */
}

/* The name under which dir lists the inode numbered ino. O(entries). */
static int name_in_dir(Inode *dir, uint64_t ino, char name[VFS_NAME_MAX + 1]) {
    File *f = file_new(igrab(dir), dir->fop, O_RDONLY | O_DIRECTORY);
    if (f == NULL) {
        iput(dir);
        return -ENOMEM;
    }
    int err = 0;
    if (f->fop != NULL && f->fop->open != NULL) {
        err = f->fop->open(dir, f);
        if (err != 0) f->fop = NULL;   /* open failed: no release */
    }
    FindIno fi = {.ctx = {.actor = find_ino_actor}, .ino = ino, .name = name};
    if (err == 0) err = vfs_iterate(f, &fi.ctx);
    fput(f);
    if (err == 0 && !fi.found) err = -ENOENT;
    return err;
}

/* Put s (n bytes) in front of buf[*pos..]. -ERANGE when it doesn't fit. */
static int prepend(char *buf, size_t *pos, const char *s, size_t n) {
    if (n > *pos) return -ERANGE;
    *pos -= n;
    memcpy(buf + *pos, s, n);
    return 0;
}

int vfs_path_of(const PathLoc *loc, char *buf, size_t size) {
    if (size < 2) return -ERANGE;
    size_t pos = size - 1;
    buf[pos] = '\0';
    PathLoc cur;
    loc_get(&cur, loc);
    int err = 0;
    char name[VFS_NAME_MAX + 1];
    for (uint32_t depth = 0; err == 0; depth++) {
        if (depth > PATH_MAX / 2) {
            err = -ENAMETOOLONG;   /* a loop of ".." entries (a corrupt disk) */
            break;
        }
        if (cur.inode == cur.mnt->root) {
            /* "/", or a mount: its path is where the rest comes from */
            if (cur.mnt->parent != NULL) err = prepend(buf, &pos, cur.mnt->path, strlen(cur.mnt->path));
            break;
        }
        Inode *parent;
        err = lookup_dotdot(cur.inode, &parent);
        if (err != 0) break;
        err = name_in_dir(parent, cur.inode->ino, name);
        if (err == 0) err = prepend(buf, &pos, name, strlen(name));
        if (err == 0) err = prepend(buf, &pos, "/", 1);
        iput(cur.inode);
        cur.inode = parent;
    }
    loc_put(&cur);
    if (err == 0 && pos == size - 1) err = prepend(buf, &pos, "/", 1);
    if (err != 0) return err;
    size_t len = size - 1 - pos;
    memmove(buf, buf + pos, len + 1);
    return (int)len;
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

/* One try of open(O_CREAT) in dir: the existing entry, or a new file. */
static int open_create_in(Inode *dir, const char *name, int slash, uint32_t flags, uint32_t mode, Inode **inode,
                          int *created) {
    int err = inode_permission(dir, X_OK);
    if (err != 0) return err;
    rwsem_down_write(&dir->lock);
    err = lookup_locked(dir, name, inode);
    if (err == 0 && (flags & O_EXCL)) {
        iput(*inode);
        err = -EEXIST;
    } else if (err == -ENOENT) {
        err = slash ? -EISDIR : vfs_create_locked(dir, name, mode, inode);
        *created = (err == 0);
    }
    rwsem_up_write(&dir->lock);
    return err;
}

int vfs_open_locate(const PathLoc *base, const char *path, uint32_t flags, uint32_t mode, PathLoc *out, int *created) {
    *created = 0;
    if (!(flags & O_CREAT)) {
        int err = vfs_resolve_at(base, path, (flags & O_NOFOLLOW) ? 0 : WALK_FOLLOW, out);
        if (err == 0 && S_ISLNK(out->inode->mode)) {   /* O_NOFOLLOW on a symlink */
            loc_put(out);
            err = -ELOOP;
        }
        return err;
    }
    /* O_CREAT: the last component is looked up (or made) under its
     * directory's lock; a symlink there is followed by trying again with its
     * target, relative to that directory (a dangling one creates its target,
     * as on Linux) */
    PathLoc at = {NULL, NULL};   /* the directory of the last link followed */
    char *target = NULL;
    const char *p = path;
    int err;
    for (uint32_t links = 0;; links++) {
        char name[VFS_NAME_MAX + 1];
        int slash;
        PathLoc dir;
        err = vfs_resolve_parent_at(at.inode != NULL ? &at : base, p, &dir, name, &slash);
        if (err == -EINVAL) err = -EISDIR;   /* "/" */
        if (err != 0) break;
        Inode *inode = NULL;
        err = is_dots(name) ? -EISDIR : open_create_in(dir.inode, name, slash, flags, mode, &inode, created);
        if (err == 0 && S_ISLNK(inode->mode)) {
            if ((flags & O_NOFOLLOW) || links >= VFS_MAX_LINKS) {
                err = -ELOOP;
            } else if (target == NULL && (target = kmalloc(PATH_MAX, 0)) == NULL) {
                err = -ENOMEM;
            } else {
                err = link_target(inode, target, slash ? "/" : "");
                if (err > 0) {
                    iput(inode);
                    loc_put(&at);
                    at = dir;   /* the next try starts in the link's directory */
                    p = target;
                    continue;
                }
            }
            iput(inode);
        } else if (err == 0) {
            out->mnt = dir.mnt;
            out->inode = inode;
            cross_mounts(out);
            if (S_ISDIR(out->inode->mode) || (slash && !S_ISDIR(out->inode->mode))) {
                err = S_ISDIR(out->inode->mode) ? -EISDIR : -ENOTDIR;   /* Linux: O_CREAT on a directory */
                loc_put(out);
            }
        }
        loc_put(&dir);
        break;
    }
    loc_put(&at);
    kfree(target);
    return err;
}

int vfs_mkdir_at(const PathLoc *base, const char *path, uint32_t mode) {
    char name[VFS_NAME_MAX + 1];
    int slash;
    PathLoc dir;
    int err = vfs_resolve_parent_at(base, path, &dir, name, &slash);
    if (err == -EINVAL) err = -EEXIST;   /* "/" */
    if (err != 0) return err;
    Inode *d = dir.inode;
    if (is_dots(name)) {
        loc_put(&dir);
        return -EEXIST;
    }
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

int vfs_unlink_at(const PathLoc *base, const char *path) {
    char name[VFS_NAME_MAX + 1];
    int slash;
    PathLoc dir;
    int err = vfs_resolve_parent_at(base, path, &dir, name, &slash);
    if (err == -EINVAL) err = -EISDIR;   /* "/" */
    if (err != 0) return err;
    if (is_dots(name)) {
        loc_put(&dir);
        return -EISDIR;
    }
    Inode *d = dir.inode;
    Cred c = current_cred();
    Inode *victim = NULL;
    rwsem_down_write(&d->lock);
    err = lookup_locked(d, name, &victim);
    if (err == 0 && S_ISDIR(victim->mode)) err = -EISDIR;
    if (err == 0 && slash) err = -ENOTDIR;   /* "file/" (Linux) */
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

int vfs_rmdir_at(const PathLoc *base, const char *path) {
    char name[VFS_NAME_MAX + 1];
    int slash;
    PathLoc dir;
    int err = vfs_resolve_parent_at(base, path, &dir, name, &slash);
    if (err == -EINVAL) err = -EBUSY;   /* "/" */
    if (err != 0) return err;
    if (is_dots(name)) {   /* Linux: "." is -EINVAL, ".." -ENOTEMPTY */
        loc_put(&dir);
        return name[1] == '\0' ? -EINVAL : -ENOTEMPTY;
    }
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

/* Is anc a proper ancestor of the directory desc, both in mount m? Walks
 * ".." up from desc (with rename_mutex held no directory moves meanwhile).
 * 1 with *below = anc's child on the way (a reference), 0, or -errno. */
static int find_ancestor(Mount *m, Inode *anc, Inode *desc, Inode **below) {
    Inode *cur = igrab(desc), *prev = NULL;
    int err = 0;
    for (uint32_t depth = 0; cur != anc && cur != m->root; depth++) {
        Inode *up;
        if (depth > PATH_MAX / 2) {
            err = -ELOOP;   /* a loop of ".." entries (a corrupt disk) */
            break;
        }
        err = lookup_dotdot(cur, &up);
        if (err != 0) break;
        if (prev != NULL) iput(prev);
        prev = cur;
        cur = up;
        if (cur == prev) break;   /* a filesystem root that is its own parent */
    }
    int found = err == 0 && cur == anc && prev != NULL;
    if (found) {
        *below = prev;
        prev = NULL;
    }
    if (prev != NULL) iput(prev);
    iput(cur);
    return err != 0 ? err : found;
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

/* How the two parents of a rename relate: below_o is odir's child on the way
 * down to ndir when odir is an ancestor of ndir; below_n likewise for ndir
 * over odir (each a reference, or NULL). */
typedef struct {
    Inode *below_o;
    Inode *below_n;
} RenameAncestry;

/* The rename itself, the parents resolved and in the same mount, and their
 * ancestry known: lock, look up, check, call the filesystem. */
static int rename_locked(Inode *odir, const char *oname, Inode *ndir, const char *nname, int slash,
                         const RenameAncestry *a) {
    Cred c = current_cred();
    Inode *src = NULL, *victim = NULL;
    if (odir == ndir) {
        rwsem_down_write(&odir->lock);
    } else {
        Inode *first = a->below_n != NULL ? ndir : odir;   /* the ancestor, else either */
        rwsem_down_write(&first->lock);
        rwsem_down_write_nested(&(first == odir ? ndir : odir)->lock, LOCK_LEVEL_INODE_2);
    }
    int err = lookup_locked(odir, oname, &src);
    if (err == 0) {
        err = lookup_locked(ndir, nname, &victim);
        if (err == -ENOENT) err = 0;
    }
    int locked_src = 0, locked_victim = 0;
    if (err == 0 && src != victim) {
        if (slash && !S_ISDIR(src->mode)) err = -ENOTDIR;
        /* The target contains the source (rename("/a/b/c", "/a/b")): not empty */
        if (err == 0 && victim != NULL && victim == a->below_n) err = -ENOTEMPTY;
        /* A directory into itself or below it */
        if (err == 0 && S_ISDIR(src->mode) && src == a->below_o) err = -EINVAL;
        if (err == 0) err = rename_check(odir, src, ndir, victim, &c);
        if (err == 0 && (odir->iop == NULL || odir->iop->rename == NULL)) err = -EPERM;
        if (err == 0 && S_ISDIR(src->mode)) {   /* its ".." changes */
            rwsem_down_write_nested(&src->lock, LOCK_LEVEL_INODE_CHILD);
            locked_src = 1;
        }
        if (err == 0 && victim != NULL && S_ISDIR(victim->mode)) {
            rwsem_down_write_nested(&victim->lock, LOCK_LEVEL_INODE_CHILD_2);
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
    if (locked_src) rwsem_up_write(&src->lock);
    if (ndir != odir) rwsem_up_write(&ndir->lock);
    rwsem_up_write(&odir->lock);
    if (src != NULL) iput(src);
    if (victim != NULL) iput(victim);
    return err;
}

/* The rename, the parents resolved and in the same mount. */
static int rename_in(PathLoc *od, const char *oname, PathLoc *nd, const char *nname, int slash) {
    Inode *odir = od->inode, *ndir = nd->inode;
    if (odir->sb->flags & SB_RDONLY) return -EROFS;   /* before any ".." walk reads the disk */
    RenameAncestry a = {NULL, NULL};
    int err = 0;
    if (odir != ndir) {
        err = find_ancestor(od->mnt, odir, ndir, &a.below_o);
        if (err == 0) err = find_ancestor(nd->mnt, ndir, odir, &a.below_n);
        if (err > 0) err = 0;
    }
    if (err == 0) err = rename_locked(odir, oname, ndir, nname, slash, &a);
    if (a.below_o != NULL) iput(a.below_o);
    if (a.below_n != NULL) iput(a.below_n);
    return err;
}

int vfs_rename_at(const PathLoc *obase, const char *oldpath, const PathLoc *nbase, const char *newpath) {
    char oname[VFS_NAME_MAX + 1], nname[VFS_NAME_MAX + 1];
    int oslash, nslash;
    PathLoc od, nd;
    mutex_lock(&rename_mutex);   /* before resolving: no directory moves until we're done */
    int err = vfs_resolve_parent_at(obase, oldpath, &od, oname, &oslash);
    if (err == -EINVAL) err = -EBUSY; /* "/" */
    if (err != 0) {
        mutex_unlock(&rename_mutex);
        return err;
    }
    err = vfs_resolve_parent_at(nbase, newpath, &nd, nname, &nslash);
    if (err == -EINVAL) err = -EBUSY;
    if (err == 0) {
        if (is_dots(oname) || is_dots(nname))
            err = -EBUSY;   /* Linux */
        else
            err = od.mnt != nd.mnt ? -EXDEV : rename_in(&od, oname, &nd, nname, oslash || nslash);
        loc_put(&nd);
    }
    loc_put(&od);
    mutex_unlock(&rename_mutex);
    return err;
}

int vfs_stat_at(const PathLoc *base, const char *path, uint32_t flags, struct stat *st) {
    PathLoc loc;
    int err = vfs_resolve_at(base, path, flags, &loc);
    if (err != 0) return err;
    vfs_fill_stat(loc.inode, st);
    loc_put(&loc);
    return 0;
}

int vfs_readlink_at(const PathLoc *base, const char *path, char *buf, size_t size) {
    PathLoc loc;
    int err = vfs_resolve_at(base, path, 0, &loc);
    if (err != 0) return err;
    Inode *inode = loc.inode;
    if (!S_ISLNK(inode->mode))
        err = -EINVAL;
    else if (inode->iop == NULL || inode->iop->readlink == NULL)
        err = -EIO;
    else
        err = inode->iop->readlink(inode, buf, size);
    if (err > (int)size) err = (int)size;   /* readlink(2) cuts the target */
    loc_put(&loc);
    return err;
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
