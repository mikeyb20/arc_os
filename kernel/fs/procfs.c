#include "fs/procfs.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "arch/x86_64/pit.h"
#include "proc/process.h"
#include "lib/mem.h"
#include "lib/string.h"

/* Scratch buffer for content generation */
#define PROCFS_SCRATCH_SIZE 512

/* --- Formatting helpers (no snprintf available) --- */

static int procfs_append_str(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max - 1) {
        buf[pos++] = *s++;
    }
    return pos;
}

static int procfs_append_u64(char *buf, int pos, int max, uint64_t val) {
    char tmp[21];
    int len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0 && len < 20) {
            tmp[len++] = '0' + (val % 10);
            val /= 10;
        }
    }
    for (int i = len - 1; i >= 0 && pos < max - 1; i--) {
        buf[pos++] = tmp[i];
    }
    return pos;
}

/* --- Content generators --- */

typedef int (*procfs_gen_fn)(char *buf, int bufsz, void *ctx);

static int gen_meminfo(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    int pos = 0;
    uint64_t total_pages = pmm_get_total_pages();
    uint64_t free_pages = pmm_get_free_pages();
    uint64_t total_kb = (total_pages * PAGE_SIZE) / 1024;
    uint64_t free_kb = (free_pages * PAGE_SIZE) / 1024;
    HeapStats hs;
    kmalloc_get_stats(&hs);

    pos = procfs_append_str(buf, pos, bufsz, "MemTotal: ");
    pos = procfs_append_u64(buf, pos, bufsz, total_kb);
    pos = procfs_append_str(buf, pos, bufsz, " kB\nMemFree: ");
    pos = procfs_append_u64(buf, pos, bufsz, free_kb);
    pos = procfs_append_str(buf, pos, bufsz, " kB\nHeapUsed: ");
    pos = procfs_append_u64(buf, pos, bufsz, hs.total_used);
    pos = procfs_append_str(buf, pos, bufsz, " B\nHeapFree: ");
    pos = procfs_append_u64(buf, pos, bufsz, hs.total_free);
    pos = procfs_append_str(buf, pos, bufsz, " B\nHeapMapped: ");
    pos = procfs_append_u64(buf, pos, bufsz, hs.heap_mapped);
    pos = procfs_append_str(buf, pos, bufsz, " B\n");

    return pos;
}

static int gen_uptime(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    int pos = 0;
    uint64_t ms = pit_get_uptime_ms();
    uint64_t secs = ms / 1000;
    uint64_t frac = ms % 1000;

    pos = procfs_append_u64(buf, pos, bufsz, secs);
    pos = procfs_append_str(buf, pos, bufsz, ".");
    if (frac < 100) {
        pos = procfs_append_str(buf, pos, bufsz, "0");
        if (frac < 10) {
            pos = procfs_append_str(buf, pos, bufsz, "0");
        }
    }
    pos = procfs_append_u64(buf, pos, bufsz, frac);
    pos = procfs_append_str(buf, pos, bufsz, "\n");

    return pos;
}

static int gen_pid_status(char *buf, int bufsz, void *ctx) {
    uint32_t pid = (uint32_t)(uintptr_t)ctx;
    Process *p = proc_get_by_pid(pid);
    if (p == NULL) return 0;

    int pos = 0;
    pos = procfs_append_str(buf, pos, bufsz, "Pid: ");
    pos = procfs_append_u64(buf, pos, bufsz, p->pid);
    pos = procfs_append_str(buf, pos, bufsz, "\nState: ");

    const char *state_str;
    switch (p->state) {
    case PROC_ALIVE:      state_str = "running"; break;
    case PROC_ZOMBIE:     state_str = "zombie"; break;
    case PROC_TERMINATED: state_str = "terminated"; break;
    case PROC_STOPPED:    state_str = "stopped"; break;
    default:              state_str = "unknown"; break;
    }
    pos = procfs_append_str(buf, pos, bufsz, state_str);

    pos = procfs_append_str(buf, pos, bufsz, "\nPPid: ");
    pos = procfs_append_u64(buf, pos, bufsz, p->parent ? p->parent->pid : 0);
    pos = procfs_append_str(buf, pos, bufsz, "\nPgid: ");
    pos = procfs_append_u64(buf, pos, bufsz, p->pgid);
    pos = procfs_append_str(buf, pos, bufsz, "\nUid: ");
    pos = procfs_append_u64(buf, pos, bufsz, p->uid);
    pos = procfs_append_str(buf, pos, bufsz, "\nGid: ");
    pos = procfs_append_u64(buf, pos, bufsz, p->gid);
    pos = procfs_append_str(buf, pos, bufsz, "\n");

    return pos;
}

/* --- Node types --- */

typedef struct {
    VfsNode       vnode;
    procfs_gen_fn generate;
    void         *gen_ctx;
} ProcfsFileNode;

typedef struct {
    VfsNode   vnode;
    uint32_t  pid;
} ProcfsDirNode;

/* --- File read op --- */

static int procfs_file_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size) {
    ProcfsFileNode *pn = (ProcfsFileNode *)node;
    if (pn->generate == NULL) return 0;

    char scratch[PROCFS_SCRATCH_SIZE];
    int total = pn->generate(scratch, PROCFS_SCRATCH_SIZE, pn->gen_ctx);
    if (total <= 0) return 0;

    if (offset >= (uint32_t)total) return 0;
    uint32_t avail = (uint32_t)total - offset;
    uint32_t to_copy = size < avail ? size : avail;
    memcpy(buf, scratch + offset, to_copy);

    return (int)to_copy;
}

static const VfsOps procfs_file_ops = {
    .read = procfs_file_read,
};

/* --- Static node pools --- */

static ProcfsFileNode meminfo_node;
static ProcfsFileNode uptime_node;

#define PROCFS_PID_POOL 8
static ProcfsDirNode pid_dirs[PROCFS_PID_POOL];
static int pid_dirs_next;
static ProcfsFileNode pid_status_nodes[PROCFS_PID_POOL];
static int pid_status_next;

static ProcfsDirNode procfs_root_node;

/* --- Directory ops for /proc/[pid] --- */

static VfsNode *procfs_pid_lookup(VfsNode *dir, const char *name) {
    ProcfsDirNode *pd = (ProcfsDirNode *)dir;
    if (strcmp(name, "status") != 0) return NULL;

    int slot = pid_status_next % PROCFS_PID_POOL;
    pid_status_next++;

    ProcfsFileNode *fn = &pid_status_nodes[slot];
    fn->vnode.inode_num = 3000 + pd->pid * 10 + 1;
    fn->vnode.type = VFS_FILE;
    fn->vnode.size = 0;
    fn->vnode.mode = 0444;
    fn->vnode.ops = &procfs_file_ops;
    fn->vnode.private_data = fn;
    fn->generate = gen_pid_status;
    fn->gen_ctx = (void *)(uintptr_t)pd->pid;

    return &fn->vnode;
}

static int procfs_pid_readdir(VfsNode *dir, VfsDirEntry *entries, uint32_t max) {
    (void)dir;
    if (max < 1) return 0;
    strncpy(entries[0].name, "status", VFS_NAME_MAX - 1);
    entries[0].name[VFS_NAME_MAX - 1] = '\0';
    entries[0].inode_num = 0;
    entries[0].type = VFS_FILE;
    return 1;
}

static const VfsOps procfs_pid_dir_ops = {
    .lookup  = procfs_pid_lookup,
    .readdir = procfs_pid_readdir,
};

/* --- Directory ops for /proc root --- */

static int procfs_parse_uint(const char *s, uint32_t *out) {
    if (*s == '\0') return -1;
    uint32_t val = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        val = val * 10 + (uint32_t)(*s - '0');
        s++;
    }
    *out = val;
    return 0;
}

static VfsNode *procfs_root_lookup(VfsNode *dir, const char *name) {
    (void)dir;
    if (strcmp(name, "meminfo") == 0) return &meminfo_node.vnode;
    if (strcmp(name, "uptime") == 0) return &uptime_node.vnode;

    uint32_t pid;
    if (procfs_parse_uint(name, &pid) != 0) return NULL;

    Process *p = proc_get_by_pid(pid);
    if (p == NULL) return NULL;

    int slot = pid_dirs_next % PROCFS_PID_POOL;
    pid_dirs_next++;

    ProcfsDirNode *pd = &pid_dirs[slot];
    pd->vnode.inode_num = 2000 + pid;
    pd->vnode.type = VFS_DIRECTORY;
    pd->vnode.size = 0;
    pd->vnode.mode = 0555;
    pd->vnode.ops = &procfs_pid_dir_ops;
    pd->vnode.private_data = pd;
    pd->pid = pid;

    return &pd->vnode;
}

/* Callback for proc_foreach to enumerate PIDs */
struct procfs_readdir_ctx {
    VfsDirEntry *entries;
    uint32_t max;
    uint32_t count;
};

static void procfs_enum_pid(Process *p, void *ctx) {
    struct procfs_readdir_ctx *rc = (struct procfs_readdir_ctx *)ctx;
    if (rc->count >= rc->max) return;

    /* Convert PID to decimal string */
    char pidbuf[12];
    int pos = 0;
    uint32_t pid = p->pid;
    char tmp[12];
    int len = 0;
    if (pid == 0) {
        tmp[len++] = '0';
    } else {
        while (pid > 0 && len < 10) {
            tmp[len++] = '0' + (pid % 10);
            pid /= 10;
        }
    }
    for (int i = len - 1; i >= 0; i--) {
        pidbuf[pos++] = tmp[i];
    }
    pidbuf[pos] = '\0';

    VfsDirEntry *e = &rc->entries[rc->count];
    strncpy(e->name, pidbuf, VFS_NAME_MAX - 1);
    e->name[VFS_NAME_MAX - 1] = '\0';
    e->inode_num = 2000 + p->pid;
    e->type = VFS_DIRECTORY;
    rc->count++;
}

static int procfs_root_readdir(VfsNode *dir, VfsDirEntry *entries, uint32_t max) {
    (void)dir;
    uint32_t count = 0;

    if (count < max) {
        strncpy(entries[count].name, "meminfo", VFS_NAME_MAX - 1);
        entries[count].name[VFS_NAME_MAX - 1] = '\0';
        entries[count].inode_num = meminfo_node.vnode.inode_num;
        entries[count].type = VFS_FILE;
        count++;
    }
    if (count < max) {
        strncpy(entries[count].name, "uptime", VFS_NAME_MAX - 1);
        entries[count].name[VFS_NAME_MAX - 1] = '\0';
        entries[count].inode_num = uptime_node.vnode.inode_num;
        entries[count].type = VFS_FILE;
        count++;
    }

    struct procfs_readdir_ctx ctx = { entries, max, count };
    proc_foreach(procfs_enum_pid, &ctx);

    return (int)ctx.count;
}

static const VfsOps procfs_root_dir_ops = {
    .lookup  = procfs_root_lookup,
    .readdir = procfs_root_readdir,
};

/* --- Init --- */

static void procfs_init_file(ProcfsFileNode *fn, uint64_t ino,
                             procfs_gen_fn gen, void *ctx) {
    fn->vnode.inode_num = ino;
    fn->vnode.type = VFS_FILE;
    fn->vnode.size = 0;
    fn->vnode.mode = 0444;
    fn->vnode.ops = &procfs_file_ops;
    fn->vnode.private_data = fn;
    fn->generate = gen;
    fn->gen_ctx = ctx;
}

VfsNode *procfs_init(void) {
    procfs_root_node.vnode.inode_num = 2000;
    procfs_root_node.vnode.type = VFS_DIRECTORY;
    procfs_root_node.vnode.size = 0;
    procfs_root_node.vnode.mode = 0555;
    procfs_root_node.vnode.ops = &procfs_root_dir_ops;
    procfs_root_node.vnode.private_data = &procfs_root_node;
    procfs_root_node.pid = 0;

    procfs_init_file(&meminfo_node, 2001, gen_meminfo, NULL);
    procfs_init_file(&uptime_node, 2002, gen_uptime, NULL);

    pid_dirs_next = 0;
    pid_status_next = 0;

    return &procfs_root_node.vnode;
}
