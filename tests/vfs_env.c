/* arc_os — host stand-ins for the kernel around the VFS (see vfs_env.h) */

#include "vfs_env.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/devfs.h"
#include "fs/procfs.h"
#include "fs/ramfs.h"
#include "fs/vfs_internal.h"
#include "drivers/irq.h"
#include "drivers/tty.h"
#include "lib/klog.h"
#include "mm/kmalloc.h"
#include "mm/mm.h"
#include "mm/filemap.h"
#include "mm/page.h"
#include "mm/pmm.h"
#include "mm/slab.h"
#include "mm/vmm.h"
#include "proc/mutex.h"
#include "proc/rwsem.h"
#include "proc/sched.h"
#include "proc/waitqueue.h"
#include "user_access.h"
#include "arch/x86_64/cpu.h"
#include "uapi/errno.h"
#include "uapi/termios.h"
#include "uapi/fcntl.h"

/* --- Interrupt state the host spinlocks keep (arch/hal_lock.h) --- */
_Thread_local int hal_test_irq_enabled = 1;
_Thread_local uint32_t hal_test_lock_depth;

/* --- Allocation failure injection --- */
static int fail_in;

static int should_fail(void) {
    if (fail_in == 0) return 0;
    if (--fail_in == 0) return 1;
    return 0;
}

void vfs_env_fail_alloc_in(int n) {
    fail_in = n;
}

/* --- kmalloc over malloc: a size header keeps the live byte count --- */
static int64_t live_kmalloc;
static int64_t live_slab;
static int64_t live_pages;

void *kmalloc(size_t size, uint32_t flags) {
    if (should_fail()) return NULL;
    size_t *p = malloc(size + 16);
    if (p == NULL) return NULL;
    p[0] = size;
    p[1] = 0x6b6d616c6c6f63ull;   /* "kmalloc" */
    if (flags & GFP_ZERO) memset(p + 2, 0, size);
    live_kmalloc += (int64_t)size;
    return p + 2;
}

void kfree(void *ptr) {
    if (ptr == NULL) return;
    size_t *p = (size_t *)ptr - 2;
    if (p[1] == 0x6b6d616c6c6f63ull) {
        live_kmalloc -= (int64_t)p[0];
        p[1] = 0;
        free(p);
        return;
    }
    /* a slab object: kfree takes those too */
    p = (size_t *)ptr - 2;
    if (p[1] == 0x736c6162ull) {
        live_slab--;
        p[1] = 0;
        free(p);
        return;
    }
    fprintf(stderr, "vfs_env: kfree of a pointer kmalloc didn't hand out\n");
    abort();
}

void *krealloc(void *ptr, size_t new_size) {
    void *n = kmalloc(new_size, 0);
    if (n == NULL) return NULL;
    if (ptr != NULL) {
        size_t old = ((size_t *)ptr - 2)[0];
        memcpy(n, ptr, old < new_size ? old : new_size);
        kfree(ptr);
    }
    return n;
}

void kmalloc_get_stats(HeapStats *out) {
    memset(out, 0, sizeof(*out));
    out->total_used = (size_t)live_kmalloc;
}

/* --- Slab caches over malloc --- */
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align) {
    (void)align;
    struct kmem_cache *c = calloc(1, sizeof(*c));
    c->name = name;
    c->object_size = (uint32_t)size;
    return c;
}

void *kmem_cache_alloc(struct kmem_cache *cache, uint32_t flags) {
    if (should_fail()) return NULL;
    size_t *p = malloc(cache->object_size + 16);
    if (p == NULL) return NULL;
    p[0] = (size_t)cache;
    p[1] = 0x736c6162ull;   /* "slab" */
    /* Kernel slab memory is not zeroed unless asked: poison it */
    memset(p + 2, (flags & GFP_ZERO) ? 0 : 0xa5, cache->object_size);
    live_slab++;
    return p + 2;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj) {
    if (obj == NULL) return;
    size_t *p = (size_t *)obj - 2;
    if (p[1] != 0x736c6162ull || p[0] != (size_t)cache) {
        fprintf(stderr, "vfs_env: kmem_cache_free of a foreign or freed object\n");
        abort();
    }
    p[1] = 0;
    live_slab--;
    free(p);
}

int slab_get_info(unsigned n, SlabInfo *out) {
    if (n > 0) return -1;
    memset(out, 0, sizeof(*out));
    out->name = "host";
    out->object_size = 64;
    return 0;
}

/* --- Page arena: PFN 0 is the arena's first page --- */
#define ARENA_PAGES 1024
struct page *pmm_page_array;
uint64_t pmm_hhdm_offset;
static uint8_t *arena;
static uint8_t page_used[ARENA_PAGES];

static void arena_init(void) {
    if (arena != NULL) return;
    arena = aligned_alloc(PAGE_SIZE, (size_t)ARENA_PAGES * PAGE_SIZE);
    pmm_page_array = calloc(ARENA_PAGES, sizeof(struct page));
    pmm_hhdm_offset = (uint64_t)(uintptr_t)arena;
}

void (*vfs_env_alloc_hook)(void);

struct page *alloc_pages(unsigned order, uint32_t flags) {
    if (vfs_env_alloc_hook != NULL) vfs_env_alloc_hook();
    arena_init();
    if (order != 0 || should_fail()) return NULL;
    for (uint32_t i = 0; i < ARENA_PAGES; i++) {
        if (page_used[i]) continue;
        page_used[i] = 1;
        live_pages++;
        memset(arena + (size_t)i * PAGE_SIZE, (flags & GFP_ZERO) ? 0 : 0xee, PAGE_SIZE);
        pmm_page_array[i].refcount = 1;
        return &pmm_page_array[i];
    }
    return NULL;
}

void free_pages(struct page *page, unsigned order) {
    (void)order;
    uint64_t pfn = page_to_pfn(page);
    if (pfn >= ARENA_PAGES || !page_used[pfn]) {
        fprintf(stderr, "vfs_env: free_pages of a free page\n");
        abort();
    }
    if (page->refcount == 0) {
        fprintf(stderr, "vfs_env: free_pages of a page with no references\n");
        abort();
    }
    if (--page->refcount != 0) return;   /* as the PMM's: the last reference frees */
    page->flags = 0;
    page_used[pfn] = 0;
    live_pages--;
}

uint64_t pmm_get_total_pages(void) {
    return ARENA_PAGES;
}

uint64_t pmm_get_free_pages(void) {
    return ARENA_PAGES - (uint64_t)live_pages;
}

uint64_t pmm_get_descriptor_bytes(void) {
    return ARENA_PAGES * sizeof(struct page);
}

void vfs_env_usage(VfsEnvUsage *u) {
    u->kmalloc_bytes = live_kmalloc;
    u->slab_objects = live_slab;
    u->pages = live_pages;
    u->dentries = dcache_count();
}

int vfs_env_no_leak(const VfsEnvUsage *a, const VfsEnvUsage *b) {
    int64_t kb = b->kmalloc_bytes - a->kmalloc_bytes;
    int64_t objs = (b->slab_objects - b->dentries) - (a->slab_objects - a->dentries);
    int64_t pages = b->pages - a->pages;
    if (kb == 0 && objs == 0 && pages == 0) return 1;
    printf("    leaked: kmalloc %+lld B, slab %+lld objects (dentries aside), pages %+lld\n", (long long)kb,
           (long long)objs, (long long)pages);
    return 0;
}

/* --- User copies: low addresses fault, and so does vfs_env_fault_at on --- */
uintptr_t vfs_env_fault_at;

/* Bytes of [p, p + len) before the first faulting one */
static size_t uaddr_ok_len(const void *p, size_t len) {
    uintptr_t a = (uintptr_t)p;
    if (a < 4096) return 0;
    if (vfs_env_fault_at != 0 && a + len > vfs_env_fault_at) return a >= vfs_env_fault_at ? 0 : vfs_env_fault_at - a;
    return len;
}

static int uaddr_bad(const void *p, size_t len) {
    return len != 0 && uaddr_ok_len(p, len) < len;
}

int copy_from_user(void *dst, const void *src, size_t len) {
    if (uaddr_bad(src, len)) {
        memset(dst, 0, len);   /* as the kernel's: the uncopied tail is zeroed */
        return -EFAULT;
    }
    memcpy(dst, src, len);
    return 0;
}

size_t copy_from_user_partial(void *dst, const void *src, size_t len) {
    size_t ok = uaddr_ok_len(src, len);
    memcpy(dst, src, ok);
    return ok;
}

int copy_to_user(void *dst, const void *src, size_t len) {
    if (uaddr_bad(dst, len)) return -EFAULT;
    memcpy(dst, src, len);
    return 0;
}

size_t copy_from_user_nofault(void *dst, const void *src, size_t len) {
    return copy_from_user_partial(dst, src, len);
}

/* --- Page tables for the file-mapping tests (mm/filemap_mmap.c): one flat
 * list of (address space, page) entries, flags = the VMA protection --- */
#define ENV_PTES 256
static struct {
    uint64_t pgd, virt, phys;
    uint32_t flags;
    int used;
} env_ptes[ENV_PTES];
int vfs_env_tlb_flushes;

static int env_pte_find(uint64_t pgd, uint64_t virt) {
    for (int i = 0; i < ENV_PTES; i++) {
        if (env_ptes[i].used && env_ptes[i].pgd == pgd && env_ptes[i].virt == virt) return i;
    }
    return -1;
}

int vmm_lookup_in(uint64_t pgd, uint64_t virt, uint64_t *phys) {
    int i = env_pte_find(pgd, virt);
    if (i < 0) return 0;
    if (phys != NULL) *phys = env_ptes[i].phys;
    return 1;
}

int vmm_map_page_in(uint64_t pgd, uint64_t virt, uint64_t phys, uint32_t flags) {
    int i = env_pte_find(pgd, virt);
    for (int j = 0; i < 0 && j < ENV_PTES; j++) {
        if (!env_ptes[j].used) i = j;
    }
    if (i < 0) return -ENOMEM;
    env_ptes[i].pgd = pgd;
    env_ptes[i].virt = virt;
    env_ptes[i].phys = phys;
    env_ptes[i].flags = flags;
    env_ptes[i].used = 1;
    return 0;
}

int vfs_env_pte(uint64_t pgd, uint64_t virt, struct page **pg, uint32_t *flags) {
    int i = env_pte_find(pgd, virt);
    if (i < 0) return 0;
    if (pg != NULL) *pg = pfn_to_page(env_ptes[i].phys >> PAGE_SHIFT);
    if (flags != NULL) *flags = env_ptes[i].flags;
    return 1;
}

uint32_t mm_prot_to_vmm(uint32_t prot) {
    return prot;
}

void mm_flush_tlb_range(Mm *mm, uint64_t start, uint64_t end) {
    (void)mm;
    (void)start;
    (void)end;
    vfs_env_tlb_flushes++;
}

void mm_flush_tlb_local(uint64_t addr) {
    (void)addr;
}

void mm_rss_account(Mm *mm, int64_t anon, int64_t file) {
    mm->rss += (uint64_t)(anon + file);
}

/* As mm.c's: entries out, one flush, then the frames (cache pages: their
 * entries' references) */
uint64_t mm_zap_range(Mm *mm, uint64_t start, uint64_t end, int flush) {
    struct page *gone[ENV_PTES];
    uint64_t n = 0;
    for (int i = 0; i < ENV_PTES; i++) {
        if (!env_ptes[i].used || env_ptes[i].pgd != mm->pgd || env_ptes[i].virt < start || env_ptes[i].virt >= end)
            continue;
        env_ptes[i].used = 0;
        gone[n++] = pfn_to_page(env_ptes[i].phys >> PAGE_SHIFT);
    }
    if (n != 0 || flush == MM_ZAP_FLUSH_ALWAYS) vfs_env_tlb_flushes++;
    for (uint64_t i = 0; i < n; i++) {
        if (gone[i]->flags & PG_FILE) {
            filemap_page_unmapped(gone[i]);
            mm_rss_account(mm, 0, -1);
        } else {
            free_pages(gone[i], 0);
            mm_rss_account(mm, -1, 0);
        }
    }
    return n;
}

/* --- Sleeping locks: single-threaded, so they only check pairing --- */
static void lock_fail(const char *what) {
    fprintf(stderr, "vfs_env: %s\n", what);
    abort();
}

#define HOST_OWNER ((Thread *)1)

void mutex_init(Mutex *m, uint16_t level) {
    memset(m, 0, sizeof(*m));
    m->level = level;
}

void mutex_lock(Mutex *m) {
    if (m->owner != NULL) lock_fail("mutex taken twice (a deadlock on the kernel)");
    m->owner = HOST_OWNER;
}

void mutex_unlock(Mutex *m) {
    if (m->owner == NULL) lock_fail("mutex released while free");
    m->owner = NULL;
}

int mutex_trylock(Mutex *m) {
    if (m->owner != NULL) return 0;
    m->owner = HOST_OWNER;
    return 1;
}

void rwsem_init(RwSem *s, uint16_t level) {
    memset(s, 0, sizeof(*s));
    s->level = level;
}

void rwsem_down_read(RwSem *s) {
    if (s->readers < 0) lock_fail("read lock while write-held");
    s->readers++;
}

int rwsem_try_read(RwSem *s) {
    if (s->readers < 0) return 0;
    s->readers++;
    return 1;
}

void rwsem_up_read(RwSem *s) {
    if (s->readers <= 0) lock_fail("read unlock while not read-held");
    s->readers--;
}

void rwsem_down_write_nested(RwSem *s, uint16_t level) {
    if (s->readers != 0) lock_fail("write lock while held");
    s->readers = -1;
    s->writer = HOST_OWNER;
    s->write_level = level;
}

void rwsem_down_write(RwSem *s) {
    rwsem_down_write_nested(s, s->level);
}

void rwsem_up_write(RwSem *s) {
    if (s->readers != -1) lock_fail("write unlock while not write-held");
    s->readers = 0;
    s->writer = NULL;
}

int rwsem_write_held(const RwSem *s) {
    return s->readers == -1;
}

/* --- Wait queues: a sleep runs the test's hook --- */
void (*vfs_env_sleep_hook)(void);
int vfs_env_sleeps;

void wq_init(WaitQueue *wq) {
    memset(wq, 0, sizeof(*wq));
}

void wq_sleep(WaitQueue *wq, Spinlock *lock, uint64_t flags) {
    (void)wq;
    spin_unlock_irqrestore(lock, flags);
    vfs_env_sleeps++;
    if (vfs_env_sleep_hook == NULL) lock_fail("would sleep forever (no sleep hook)");
    vfs_env_sleep_hook();
}

int wq_wake(WaitQueue *wq) {
    (void)wq;
    return 0;
}

int wq_wake_all(WaitQueue *wq) {
    (void)wq;
    return 0;
}

/* --- Processes --- */
Process vfs_env_proc;

Process *proc_current(void) {
    return &vfs_env_proc;
}

Process *proc_get_by_pid(uint32_t pid) {
    return pid == vfs_env_proc.pid ? &vfs_env_proc : NULL;
}

int proc_foreach(void (*cb)(Process *p, void *ctx), void *ctx) {
    cb(&vfs_env_proc, ctx);
    return 1;
}

void vfs_env_as(uint32_t uid, uint32_t gid) {
    vfs_env_proc.uid = vfs_env_proc.euid = uid;
    vfs_env_proc.gid = vfs_env_proc.egid = gid;
}

/* --- Data sources procfs prints --- */
uint64_t vfs_env_uptime_ms = 1000;

/* Both clocks (time/clock.h) read the test's time: wall time = uptime here */
uint64_t clock_mono_ns(void) {
    return vfs_env_uptime_ms * 1000000;
}

uint64_t clock_real_ns(void) {
    return vfs_env_uptime_ms * 1000000;
}

uint64_t mm_anon_pages(void) {
    return 0;
}

uint64_t vmm_user_table_pages(void) {
    return 0;
}

void mm_get(Mm *mm) {
    (void)mm;
}

void mm_put(Mm *mm) {
    (void)mm;
}

/* /proc/interrupts text comes from the IRQ layer (test_irq.c tests it) */
int irq_format_proc(char *buf, int size) {
    return snprintf(buf, (size_t)size,
                    "            CPU0\n   0:          3  PCI-MSIX 0000:00:04.0 1 cpu0/0x30  q\n ERR: 0\n");
}

int mm_format_maps(Mm *mm, char *buf, int size) {
    (void)mm;
    return snprintf(buf, (size_t)size, "00400000-00401000 r-xp 00000000 00:00 0\n");
}

/* The host suites run as a scheduled thread: page waits take the sleeping path */
int sched_can_block(void) {
    return 1;
}

int sched_cpu_stats(uint32_t cpu, SchedCpuStats *out) {
    if (cpu > 0) return -1;
    memset(out, 0, sizeof(*out));
    out->core = "-";
    return 0;
}

static const CpuFeatures host_cpu = {.vendor = "HostCPU"};

const CpuFeatures *cpu_features(void) {
    return &host_cpu;
}

const char *cpu_core_type_name(uint8_t type) {
    (void)type;
    return "-";
}

int cpu_describe(uint32_t cpu, uint32_t *apic_id, uint8_t *core_type) {
    if (cpu > 0) return -1;
    *apic_id = 0;
    *core_type = 0;
    return 0;
}

const char *cmdline_raw(void) {
    return "arc.test=vfs";
}

static const char klog_text[] = "<6>[BOOT] host\n";

uint64_t klog_tail(void) {
    return 0;
}

size_t klog_read(uint64_t *pos, char *out, size_t n) {
    size_t len = sizeof(klog_text) - 1;
    if (*pos >= len) return 0;
    size_t k = len - *pos < n ? len - (size_t)*pos : n;
    memcpy(out, klog_text + *pos, k);
    *pos += k;
    return k;
}

/* --- Kernel printing --- */
void kprintf(const char *fmt, ...) {
    (void)fmt;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* --- Terminal --- */
char vfs_env_tty_out[4096];
size_t vfs_env_tty_out_len;
static char tty_in[1024];
static size_t tty_in_len;
static uint32_t tty_pgid;
static struct termios tty_termios;
static struct winsize tty_ws = {.ws_row = 25, .ws_col = 80};

void vfs_env_tty_input(const char *s) {
    size_t n = strlen(s);
    if (n > sizeof(tty_in) - tty_in_len) n = sizeof(tty_in) - tty_in_len;
    memcpy(tty_in + tty_in_len, s, n);
    tty_in_len += n;
}

int tty_read(void *buf, uint32_t count) {
    size_t n = tty_in_len < count ? tty_in_len : count;
    memcpy(buf, tty_in, n);
    memmove(tty_in, tty_in + n, tty_in_len - n);
    tty_in_len -= n;
    return (int)n;
}

int tty_write(const void *buf, uint32_t count) {
    size_t n = count;
    if (n > sizeof(vfs_env_tty_out) - vfs_env_tty_out_len) n = sizeof(vfs_env_tty_out) - vfs_env_tty_out_len;
    memcpy(vfs_env_tty_out + vfs_env_tty_out_len, buf, n);
    vfs_env_tty_out_len += n;
    return (int)count;
}

void tty_set_fg_pgid(uint32_t pgid) {
    tty_pgid = pgid;
}

uint32_t tty_get_fg_pgid(void) {
    return tty_pgid;
}

void tty_get_termios(struct termios *t) {
    *t = tty_termios;
}

void tty_set_termios(const struct termios *t) {
    tty_termios = *t;
}

void tty_flush_input(void) {
    tty_in_len = 0;
}

void tty_get_winsize(struct winsize *ws) {
    *ws = tty_ws;
}

void tty_set_winsize(const struct winsize *ws) {
    tty_ws = *ws;
}

/* --- Setup --- */
static void mount_on_new_dir(const char *path, SuperBlock *sb, const char *source) {
    if (vfs_mkdir(path, 0755) != 0 || vfs_mount(path, sb, source) != 0) {
        fprintf(stderr, "vfs_env: cannot mount %s\n", path);
        abort();
    }
}

void vfs_env_setup(void) {
    static int done;
    vfs_env_proc.pid = 1;
    vfs_env_proc.pgid = 1;
    vfs_env_as(0, 0);
    vfs_env_proc.umask = done ? 022 : 0;   /* boot runs with umask 0, as kmain does */
    strcpy(vfs_env_proc.cwd, "/");
    vfs_env_sleep_hook = NULL;
    fail_in = 0;
    if (done) return;
    done = 1;
    arena_init();
    filemap_init();
    vfs_init();
    if (vfs_mount_root(ramfs_create_sb(), "ramfs") != 0) {
        fprintf(stderr, "vfs_env: no root\n");
        abort();
    }
    mount_on_new_dir("/dev", devfs_init(), "devfs");
    mount_on_new_dir("/proc", procfs_init(), "proc");
    if (vfs_mkdir("/tmp", 01777) != 0) abort();
    vfs_env_proc.umask = 022;
}

int64_t vfs_env_read_file(const char *path, char *buf, size_t size) {
    File *f;
    int err = vfs_open(path, O_RDONLY, 0, &f);
    if (err != 0) return err;
    size_t done = 0;
    while (done + 1 < size) {
        IoBuf b = iob_kernel(buf + done, size - 1 - done);
        int64_t n = vfs_read(f, &b);
        if (n < 0) {
            fput(f);
            return n;
        }
        if (n == 0) break;
        done += (size_t)n;
    }
    buf[done] = '\0';
    fput(f);
    return (int64_t)done;
}
