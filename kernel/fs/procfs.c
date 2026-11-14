/* arc_os — procfs (procfs.h; M0, inodes M1-11)
 *
 * Owns: /proc. Fixed files (meminfo, uptime, cmdline, cpuinfo, slabinfo,
 * kmsg, sched_debug, mounts, interrupts, vmstat) and sys/vm/drop_caches are
 * static inodes; /proc/<pid> directories and their files are inodes made per lookup and freed with their last
 * reference, so nothing is shared between lookups and no pool can be
 * overrun. A file's text is generated whole at each read.
 *
 * Inode numbers (64-bit): 1 the root, 2.. the fixed files, 0x100.. sys/vm, (1 << 32) |
 * pid << 8 a pid directory, + 1 + i its file i. No two can collide.
 *
 * Locks: none of its own. Process lookups and the process list are lane KB's
 * (big kernel lock until M1-16). Lookups are not cached (SB_NO_DCACHE):
 * /proc/<pid> comes and goes with processes.
 *
 * Cost: lookup O(fixed files) or one pid lookup; readdir walks the process
 * list once per call. */

#include "fs/procfs.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "mm/mm.h"
#include "mm/filemap.h"
#include "mm/reclaim.h"
#include "mm/writeback.h"
#include "mm/vmm.h"
#include "arch/hal.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "drivers/irq.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/cmdline.h"
#include "lib/klog.h"
#include "lib/kprintf.h"
#include "uapi/dirent.h"

/* Scratch buffer for content generation */
#define PROCFS_SCRATCH_SIZE 2048   /* /proc/cpuinfo: ~80 bytes per CPU */
/* /proc/<pid>/maps: ~80 bytes per VMA, allocated per read; a longer map is
 * cut at this size (a later seq-file read can replace whole-file generation) */
#define PROCFS_MAPS_SIZE    (32 * 1024)
/* /proc/mounts: about 40 bytes per mount */
#define PROCFS_MOUNTS_SIZE  2048

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
    pos = procfs_append_str(buf, pos, bufsz, " kB\nAnonPages: ");
    pos = procfs_append_u64(buf, pos, bufsz, mm_anon_pages() * (PAGE_SIZE / 1024));
    FilemapStats fs;
    filemap_stats(&fs);
    pos = procfs_append_str(buf, pos, bufsz, " kB\nCached: ");
    pos = procfs_append_u64(buf, pos, bufsz, fs.file_pages * (PAGE_SIZE / 1024));
    pos = procfs_append_str(buf, pos, bufsz, " kB\nDirty: ");
    pos = procfs_append_u64(buf, pos, bufsz, fs.dirty * (PAGE_SIZE / 1024));
    WbStats wb;
    wb_stats(&wb);
    pos = procfs_append_str(buf, pos, bufsz, " kB\nWriteback: ");
    pos = procfs_append_u64(buf, pos, bufsz, wb.writeback * (PAGE_SIZE / 1024));
    ReclaimStats rs;
    reclaim_stats(&rs);
    pos = procfs_append_str(buf, pos, bufsz, " kB\nActive: ");
    pos = procfs_append_u64(buf, pos, bufsz, rs.nr_active * (PAGE_SIZE / 1024));
    pos = procfs_append_str(buf, pos, bufsz, " kB\nInactive: ");
    pos = procfs_append_u64(buf, pos, bufsz, rs.nr_inactive * (PAGE_SIZE / 1024));
    pos = procfs_append_str(buf, pos, bufsz, " kB\nMapped: ");
    pos = procfs_append_u64(buf, pos, bufsz, fs.mapped * (PAGE_SIZE / 1024));
    pos = procfs_append_str(buf, pos, bufsz, " kB\nPageTables: ");
    pos = procfs_append_u64(buf, pos, bufsz, vmm_user_table_pages() * (PAGE_SIZE / 1024));
    pos = procfs_append_str(buf, pos, bufsz, " kB\nPageDescriptors: ");
    pos = procfs_append_u64(buf, pos, bufsz, pmm_get_descriptor_bytes() / 1024);
    pos = procfs_append_str(buf, pos, bufsz, " kB\nHeapUsed: ");
    pos = procfs_append_u64(buf, pos, bufsz, hs.total_used);
    pos = procfs_append_str(buf, pos, bufsz, " B\nHeapFree: ");
    pos = procfs_append_u64(buf, pos, bufsz, hs.total_free);
    pos = procfs_append_str(buf, pos, bufsz, " B\nHeapMapped: ");
    pos = procfs_append_u64(buf, pos, bufsz, hs.heap_mapped);
    pos = procfs_append_str(buf, pos, bufsz, " B\nHeapLarge: ");
    pos = procfs_append_u64(buf, pos, bufsz, hs.large_bytes);
    pos = procfs_append_str(buf, pos, bufsz, " B\n");

    return pos;
}

/* Page-cache, reclaim and write-back counters, Linux /proc/vmstat names
 * where Linux has them (nr_file_pages ... allocstall; pgpgin in pages here,
 * not KiB); readahead, drop, watermark, lowest-free and write-back timer
 * rows are arc_os's own. */
static int gen_vmstat(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    FilemapStats fs;
    filemap_stats(&fs);
    ReclaimStats rs;
    reclaim_stats(&rs);
    WbStats wb;
    wb_stats(&wb);
    PmmWatermarks wm;
    pmm_get_watermarks(&wm);
    const struct {
        const char *name;
        uint64_t v;
    } rows[] = {
        {"nr_free_pages ", pmm_get_free_pages()},
        {"nr_file_pages ", fs.file_pages},
        {"nr_dirty ", fs.dirty},
        {"nr_writeback ", wb.writeback},
        {"nr_mapped ", fs.mapped},
        {"nr_active_file ", rs.nr_active},
        {"nr_inactive_file ", rs.nr_inactive},
        {"pgpgin ", fs.pgpgin},
        {"pgscan_kswapd ", rs.scan_kswapd},
        {"pgscan_direct ", rs.scan_direct},
        {"pgsteal_kswapd ", rs.steal_kswapd},
        {"pgsteal_direct ", rs.steal_direct},
        {"pgactivate ", rs.activate},
        {"pgdeactivate ", rs.deactivate},
        {"allocstall ", rs.allocstall},
        {"kswapd_wakeups ", rs.kswapd_wakeups},
        {"reclaim_unmapped ", rs.unmapped},
        {"reclaim_written ", rs.written},
        {"ra_pages ", fs.ra_pages},
        {"ra_batches ", fs.ra_batches},
        {"pages_dropped ", fs.dropped},
        {"watermark_min ", wm.min},
        {"watermark_low ", wm.low},
        {"watermark_high ", wm.high},
        {"free_lowest ", pmm_free_lowest()},
        {"wb_timer_armed ", (uint64_t)wb_timer_armed()},
        {"wb_timer_arms ", wb.timer_arms},
        {"wb_flushes ", wb.flushes},
        {"wb_written ", wb.written},
        {"wb_listed ", wb.listed},
        {"wb_dirty_for_disks ", fs.dirty_wb},
        {"wb_throttled ", wb.throttled},
        {"wb_dropped ", wb.dropped},
        {"wb_device_flushes ", wb.device_flushes},
    };
    int pos = 0;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        pos = procfs_append_str(buf, pos, bufsz, rows[i].name);
        pos = procfs_append_u64(buf, pos, bufsz, rows[i].v);
        pos = procfs_append_str(buf, pos, bufsz, "\n");
    }
    return pos;
}

/* One line per slab cache (M1-02b): name, object size, objects in use,
 * objects the cache's slabs hold, slabs. Not Linux's slabinfo 2.1 layout. */
static int gen_slabinfo(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    int pos = procfs_append_str(buf, 0, bufsz, "# name objsize active total slabs\n");
    SlabInfo si;
    for (unsigned n = 0; slab_get_info(n, &si) == 0; n++) {
        pos = procfs_append_str(buf, pos, bufsz, si.name);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, si.object_size);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, si.active_objs);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, si.total_objs);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, si.slabs);
        pos = procfs_append_str(buf, pos, bufsz, "\n");
    }
    return pos;
}

/* One block per online CPU. The core type (P, E, or - when the CPU is not
 * hybrid) is arc_os's own line; M1-17 adds the rest of Linux's format. */
static int gen_cpuinfo(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    int pos = 0;
    for (uint32_t cpu = 0; cpu < CPU_MAX; cpu++) {
        uint32_t apic_id;
        uint8_t core_type;
        if (cpu_describe(cpu, &apic_id, &core_type) != 0) continue;
        pos = procfs_append_str(buf, pos, bufsz, "processor\t: ");
        pos = procfs_append_u64(buf, pos, bufsz, cpu);
        pos = procfs_append_str(buf, pos, bufsz, "\nvendor_id\t: ");
        pos = procfs_append_str(buf, pos, bufsz, cpu_features()->vendor);
        pos = procfs_append_str(buf, pos, bufsz, "\napicid\t\t: ");
        pos = procfs_append_u64(buf, pos, bufsz, apic_id);
        pos = procfs_append_str(buf, pos, bufsz, "\ncore type\t: ");
        pos = procfs_append_str(buf, pos, bufsz, cpu_core_type_name(core_type));
        pos = procfs_append_str(buf, pos, bufsz, "\n\n");
    }
    return pos;
}

/* /proc/sched_debug (M1-05): one line per CPU with a run queue. The format is
 * arc_os's own (Linux's file is free-form debug text too): a header line, then
 * cpu, core type, threads queued or running, context switches, threads pulled
 * in by idle balancing, and nanoseconds idle and busy up to the CPU's last
 * switch. About 70 bytes per CPU. */
static int gen_sched_debug(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    int pos = procfs_append_str(buf, 0, bufsz, "cpu core nr_running nr_switches nr_migrations idle_ns busy_ns\n");
    for (uint32_t cpu = 0; cpu < HAL_MAX_CPUS; cpu++) {
        SchedCpuStats st;
        if (sched_cpu_stats(cpu, &st) != 0) continue;
        pos = procfs_append_u64(buf, pos, bufsz, cpu);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_str(buf, pos, bufsz, st.core);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, st.nr_running);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, st.nr_switches);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, st.nr_migrations);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, st.idle_ns);
        pos = procfs_append_str(buf, pos, bufsz, " ");
        pos = procfs_append_u64(buf, pos, bufsz, st.busy_ns);
        pos = procfs_append_str(buf, pos, bufsz, "\n");
    }
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

/* /proc/cmdline: the kernel command line plus a newline, as on Linux */
static int gen_cmdline(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    int pos = procfs_append_str(buf, 0, bufsz, cmdline_raw());
    return procfs_append_str(buf, pos, bufsz, "\n");
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

/* /proc/<pid>/maps (M1-07): the address space in Linux's format. The mm is
 * pinned while it is read (the read may sleep on its lock, and the process
 * may exit meanwhile). */
static int gen_pid_maps(char *buf, int bufsz, void *ctx) {
    Process *p = proc_get_by_pid((uint32_t)(uintptr_t)ctx);
    if (p == NULL || p->mm == NULL) return 0;
    Mm *mm = p->mm;
    mm_get(mm);
    int n = mm_format_maps(mm, buf, bufsz);
    mm_put(mm);
    return n;
}

/* /proc/mounts (M1-11): one line per mount */
static int gen_mounts(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    return vfs_format_mounts(buf, bufsz);
}

/* /proc/interrupts (M1-09): one line per device interrupt with its count on
 * each CPU, the source (PCI-MSIX, PCI-MSI, INTx), device, entry and target,
 * then ERR (interrupts nobody claimed). Linux's columns, arc_os's numbering. */
static int gen_interrupts(char *buf, int bufsz, void *ctx) {
    (void)ctx;
    return irq_format_proc(buf, bufsz);
}

/* --- Inodes --- */

#define PROC_INO_ROOT   1
#define PROC_INO_PID(p) ((1ULL << 32) | ((uint64_t)(p) << 8))
#define PROC_COOKIE0    2   /* first cookie after "." and ".." */

typedef struct {
    procfs_gen_fn generate;
    void *ctx;
    uint32_t scratch_size;   /* 0: PROCFS_SCRATCH_SIZE on the stack; else kmalloc'd */
} ProcGen;

static SuperBlock proc_sb;
static Inode proc_root;

static int64_t procfs_file_read(File *file, const IoBuf *buf, loff_t *pos) {
    const ProcGen *g = (const ProcGen *)file->inode->priv;
    if (g == NULL || g->generate == NULL) return 0;

    char stack_scratch[PROCFS_SCRATCH_SIZE];
    char *scratch = stack_scratch;
    int scratch_size = PROCFS_SCRATCH_SIZE;
    if (g->scratch_size > PROCFS_SCRATCH_SIZE) {
        scratch = kmalloc(g->scratch_size, 0);
        if (scratch == NULL) return -ENOMEM;
        scratch_size = (int)g->scratch_size;
    }
    int total = g->generate(scratch, scratch_size, g->ctx);
    int64_t n = 0;
    if (total > 0 && *pos < total) {
        size_t avail = (size_t)(total - *pos);
        n = (int64_t)(buf->len < avail ? buf->len : avail);
        if (iob_copy_out(buf, 0, scratch + *pos, (size_t)n) != 0) n = -EFAULT;
    }
    if (scratch != stack_scratch) kfree(scratch);
    if (n > 0) *pos += n;
    return n;
}

static const FileOps procfs_file_fops = {
    .read = procfs_file_read,
};

/* /proc/kmsg (M1-04): the kernel log ring as Linux /proc/kmsg text ("<6>..."
 * per line), read without consuming it. Offset 0 is the oldest byte still in
 * the ring, so a reader may skip bytes the ring overwrites meanwhile. */
static int64_t procfs_kmsg_read(File *file, const IoBuf *buf, loff_t *pos) {
    (void)file;
    char chunk[512];
    size_t done = 0;
    while (done < buf->len) {
        uint64_t at = klog_tail() + (uint64_t)*pos;
        size_t want = buf->len - done < sizeof(chunk) ? buf->len - done : sizeof(chunk);
        size_t n = klog_read(&at, chunk, want);
        if (n == 0) break;
        if (iob_copy_out(buf, done, chunk, n) != 0) return done > 0 ? (int64_t)done : -EFAULT;
        done += n;
        *pos += (loff_t)n;
    }
    return (int64_t)done;
}

static const FileOps procfs_kmsg_fops = {
    .read = procfs_kmsg_read,
};

/* Fixed files in /proc, in readdir order */
static ProcGen fixed_gen[] = {
    {gen_meminfo, NULL, 0},     {gen_uptime, NULL, 0},
    {gen_cmdline, NULL, 0},     {gen_cpuinfo, NULL, 0},
    {gen_slabinfo, NULL, 0},    {NULL, NULL, 0},
    {gen_sched_debug, NULL, 0}, {gen_mounts, NULL, PROCFS_MOUNTS_SIZE},
    {gen_interrupts, NULL, 0},  {gen_vmstat, NULL, 0},
};
static const char *const fixed_names[] = {"meminfo", "uptime",      "cmdline", "cpuinfo",    "slabinfo",
                                          "kmsg",    "sched_debug", "mounts",  "interrupts", "vmstat"};
#define FIXED_COUNT (sizeof(fixed_names) / sizeof(fixed_names[0]))
#define FIXED_KMSG  5
_Static_assert(sizeof(fixed_gen) / sizeof(fixed_gen[0]) == FIXED_COUNT, "one generator per fixed file");
static Inode fixed_inodes[FIXED_COUNT];

/* Files in /proc/<pid>, in readdir order */
static const struct {
    const char *name;
    procfs_gen_fn gen;
    uint32_t scratch_size;
} pid_files[] = {
    {"status", gen_pid_status, 0},
    {"maps", gen_pid_maps, PROCFS_MAPS_SIZE},
};
#define PID_FILE_COUNT (sizeof(pid_files) / sizeof(pid_files[0]))

static void pid_evict(Inode *inode) {
    kfree(inode->priv);
    inode->priv = NULL;
}

static const SuperOps proc_sops = {.evict_inode = pid_evict};

static uint32_t pid_of(const Inode *dir) {
    return (uint32_t)((dir->ino >> 8) & 0xFFFFFF);
}

static int procfs_pid_lookup(Inode *dir, const char *name, Inode **out) {
    if (strcmp(name, "..") == 0) {
        *out = igrab(&proc_root);
        return 0;
    }
    uint32_t i = 0;
    while (i < PID_FILE_COUNT && strcmp(name, pid_files[i].name) != 0) i++;
    if (i == PID_FILE_COUNT) return -ENOENT;
    ProcGen *g = kmalloc(sizeof(*g), 0);
    Inode *inode = g != NULL ? inode_new(&proc_sb, dir->ino + 1 + i, S_IFREG | 0444) : NULL;
    if (inode == NULL) {
        kfree(g);
        return -ENOMEM;
    }
    g->generate = pid_files[i].gen;
    g->ctx = (void *)(uintptr_t)pid_of(dir);
    g->scratch_size = pid_files[i].scratch_size;
    inode->priv = g;
    inode->uid = dir->uid;
    inode->gid = dir->gid;
    inode->fop = &procfs_file_fops;
    *out = inode;
    return 0;
}

static int procfs_pid_iterate(File *file, DirContext *ctx) {
    Inode *dir = file->inode;
    if (!dir_emit_dots(ctx, dir->ino, PROC_INO_ROOT)) return 0;
    for (uint64_t i = (uint64_t)ctx->pos - PROC_COOKIE0; i < PID_FILE_COUNT; i++) {
        if (!dir_emit(ctx, pid_files[i].name, strlen(pid_files[i].name), dir->ino + 1 + i, DT_REG,
                      (loff_t)(i + 1 + PROC_COOKIE0)))
            return 0;
    }
    return 0;
}

static const InodeOps procfs_pid_iops = {.lookup = procfs_pid_lookup};
static const FileOps procfs_pid_fops = {.iterate = procfs_pid_iterate};

static int procfs_parse_uint(const char *s, uint32_t *out) {
    if (*s == '\0') return -1;
    uint32_t val = 0;
    while (*s) {
        if (*s < '0' || *s > '9' || val > 0x00FFFFFF / 10) return -1;
        val = val * 10 + (uint32_t)(*s - '0');
        s++;
    }
    *out = val;
    return 0;
}

/* --- /proc/sys/vm/drop_caches (Linux): 2 empties the dentry cache and
 * frees the disk inodes only it held (with their pages); 1 frees every clean,
 * unmapped page-cache page; 3 does both. Root only (mode 0200, as on
 * Linux). --- */

#define PROC_INO_SYS         0x100
#define PROC_INO_SYS_VM      0x101
#define PROC_INO_DROP_CACHES 0x102

/* A directory with one entry */
typedef struct {
    const char *name;
    Inode *child;
    uint8_t type;   /* DT_ */
    Inode *parent;
} ProcSubdir;

static Inode proc_sys, proc_sys_vm, proc_drop_caches;
static const ProcSubdir sys_entry = {"vm", &proc_sys_vm, DT_DIR, &proc_root};
static const ProcSubdir vm_entry = {"drop_caches", &proc_drop_caches, DT_REG, &proc_sys};

static int procfs_subdir_lookup(Inode *dir, const char *name, Inode **out) {
    const ProcSubdir *s = (const ProcSubdir *)dir->priv;
    if (strcmp(name, "..") == 0) {
        *out = igrab(s->parent);
        return 0;
    }
    if (strcmp(name, s->name) != 0) return -ENOENT;
    *out = igrab(s->child);
    return 0;
}

static int procfs_subdir_iterate(File *file, DirContext *ctx) {
    Inode *dir = file->inode;
    const ProcSubdir *s = (const ProcSubdir *)dir->priv;
    if (!dir_emit_dots(ctx, dir->ino, s->parent->ino)) return 0;
    if (ctx->pos == PROC_COOKIE0) dir_emit(ctx, s->name, strlen(s->name), s->child->ino, s->type, PROC_COOKIE0 + 1);
    return 0;
}

static int64_t procfs_drop_caches_write(File *file, const IoBuf *buf, loff_t *pos) {
    (void)file;
    char text[8];
    size_t n = buf->len < sizeof(text) - 1 ? buf->len : sizeof(text) - 1;
    if (iob_copy_in(text, buf, 0, n) != 0) return -EFAULT;
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ')) n--;
    text[n] = '\0';
    uint32_t v;
    if (procfs_parse_uint(text, &v) != 0 || v < 1 || v > 4) return -EINVAL;   /* 4: Linux's "stay quiet" bit */
    if (v & 2) vfs_drop_caches();   /* first: inodes it frees take their pages along */
    if (v & 1) (void)reclaim_drop_clean();
    *pos += (loff_t)buf->len;
    return (int64_t)buf->len;
}

static const InodeOps procfs_subdir_iops = {.lookup = procfs_subdir_lookup};
static const FileOps procfs_subdir_fops = {.iterate = procfs_subdir_iterate};
static const FileOps procfs_drop_caches_fops = {.write = procfs_drop_caches_write};

static void procfs_subdir_init(Inode *dir, uint64_t ino, const ProcSubdir *entry) {
    inode_init_static(dir, &proc_sb, ino, S_IFDIR | 0555);
    dir->nlink = entry->type == DT_DIR ? 3 : 2;
    dir->priv = (void *)entry;
    dir->iop = &procfs_subdir_iops;
    dir->fop = &procfs_subdir_fops;
}

static int procfs_root_lookup(Inode *dir, const char *name, Inode **out) {
    (void)dir;
    if (strcmp(name, "..") == 0) {   /* never asked: the VFS stops ".." at a mount root */
        *out = igrab(&proc_root);
        return 0;
    }
    if (strcmp(name, "sys") == 0) {
        *out = igrab(&proc_sys);
        return 0;
    }
    for (uint32_t i = 0; i < FIXED_COUNT; i++) {
        if (strcmp(name, fixed_names[i]) == 0) {
            *out = igrab(&fixed_inodes[i]);
            return 0;
        }
    }
    uint32_t pid;
    if (strcmp(name, "self") == 0) {   /* Linux: a symlink to the caller's pid; here the same directory */
        Process *self = proc_current();
        if (self == NULL) return -ENOENT;
        pid = self->pid;
    } else if (procfs_parse_uint(name, &pid) != 0) {
        return -ENOENT;
    }
    Process *p = proc_get_by_pid(pid);
    if (p == NULL) return -ENOENT;
    Inode *inode = inode_new(&proc_sb, PROC_INO_PID(pid), S_IFDIR | 0555);
    if (inode == NULL) return -ENOMEM;
    inode->nlink = 2;
    inode->uid = p->euid;
    inode->gid = p->egid;
    inode->iop = &procfs_pid_iops;
    inode->fop = &procfs_pid_fops;
    *out = inode;
    return 0;
}

/* Root directory listing: cookies 2.. are the fixed files, then "sys", then
 * "self", then pid + PID_COOKIE0 for each process (pids grow, so a pid cookie
 * resumes the walk where it stopped). */
#define SYS_COOKIE  (PROC_COOKIE0 + FIXED_COUNT)
#define SELF_COOKIE (SYS_COOKIE + 1)
#define PID_COOKIE0 (SELF_COOKIE + 1)

struct procfs_readdir_ctx {
    DirContext *dctx;
    int full;
};

static void procfs_enum_pid(Process *p, void *arg) {
    struct procfs_readdir_ctx *rc = (struct procfs_readdir_ctx *)arg;
    if (rc->full || (loff_t)(p->pid + PID_COOKIE0) < rc->dctx->pos) return;
    char name[12];
    int len = ksnprintf(name, sizeof(name), "%u", p->pid);
    if (!dir_emit(rc->dctx, name, (size_t)len, PROC_INO_PID(p->pid), DT_DIR, (loff_t)(p->pid + PID_COOKIE0 + 1)))
        rc->full = 1;
}

static int procfs_root_iterate(File *file, DirContext *ctx) {
    (void)file;
    if (!dir_emit_dots(ctx, PROC_INO_ROOT, PROC_INO_ROOT)) return 0;
    for (uint64_t i = (uint64_t)ctx->pos - PROC_COOKIE0; i < FIXED_COUNT; i++) {
        if (!dir_emit(ctx, fixed_names[i], strlen(fixed_names[i]), fixed_inodes[i].ino, DT_REG,
                      (loff_t)(i + 1 + PROC_COOKIE0)))
            return 0;
    }
    if (ctx->pos == SYS_COOKIE && !dir_emit(ctx, "sys", 3, PROC_INO_SYS, DT_DIR, SELF_COOKIE)) return 0;
    if (ctx->pos == SELF_COOKIE) {
        Process *self = proc_current();
        uint64_t ino = self != NULL ? PROC_INO_PID(self->pid) : PROC_INO_ROOT;
        if (!dir_emit(ctx, "self", 4, ino, DT_DIR, PID_COOKIE0)) return 0;
    }
    struct procfs_readdir_ctx rc = {ctx, 0};
    proc_foreach(procfs_enum_pid, &rc);
    return 0;
}

static const InodeOps procfs_root_iops = {.lookup = procfs_root_lookup};
static const FileOps procfs_root_fops = {.iterate = procfs_root_iterate};

SuperBlock *procfs_init(void) {
    vfs_sb_init(&proc_sb, "proc", &proc_sops);
    proc_sb.flags = SB_NO_DCACHE;
    inode_init_static(&proc_root, &proc_sb, PROC_INO_ROOT, S_IFDIR | 0555);
    proc_root.nlink = 2;
    proc_root.iop = &procfs_root_iops;
    proc_root.fop = &procfs_root_fops;
    for (uint32_t i = 0; i < FIXED_COUNT; i++) {
        inode_init_static(&fixed_inodes[i], &proc_sb, PROC_COOKIE0 + i, S_IFREG | 0444);
        fixed_inodes[i].priv = &fixed_gen[i];
        fixed_inodes[i].fop = &procfs_file_fops;
    }
    fixed_inodes[FIXED_KMSG].mode = S_IFREG | 0400;
    fixed_inodes[FIXED_KMSG].fop = &procfs_kmsg_fops;
    procfs_subdir_init(&proc_sys, PROC_INO_SYS, &sys_entry);
    procfs_subdir_init(&proc_sys_vm, PROC_INO_SYS_VM, &vm_entry);
    inode_init_static(&proc_drop_caches, &proc_sb, PROC_INO_DROP_CACHES, S_IFREG | 0200);
    proc_drop_caches.fop = &procfs_drop_caches_fops;
    proc_sb.root = &proc_root;
    return &proc_sb;
}
