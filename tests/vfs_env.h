/* arc_os — host environment for the VFS tests (M1-11)
 *
 * test_vfs_runner links the real kernel/fs sources and kernel/proc/fd.c as
 * separate objects against vfs_env.c, which stands in for the rest of the
 * kernel: kmalloc and slab caches over malloc (with live counters for leak
 * checks), a page arena for alloc_pages, user copies over memcpy (addresses
 * below 4096 fault), no-op sleeping locks that check pairing, wait queues
 * that call a test hook instead of blocking, a settable current process, a
 * fake tty and fixed procfs data sources.
 *
 * The VFS is set up once (vfs_env_setup: ramfs root with /dev, /proc and
 * /tmp); tests use their own paths under /tmp so they don't depend on each
 * other. */

#ifndef ARCHOS_TESTS_VFS_ENV_H
#define ARCHOS_TESTS_VFS_ENV_H

#include <stddef.h>
#include <stdint.h>

#include "fs/vfs.h"
#include "proc/process.h"

/* Boot the VFS once: ramfs root, /dev (devfs), /proc (procfs), /tmp 01777.
 * Later calls only reset the current process to root (uid 0, umask 022). */
void vfs_env_setup(void);

/* The process proc_current() returns (pid 1 unless changed). */
extern Process vfs_env_proc;

/* Run as uid/gid (euid/egid too). */
void vfs_env_as(uint32_t uid, uint32_t gid);

/* Live allocations: kmalloc bytes, slab objects, arena pages, and entries in
 * the dentry cache (slab objects too; at its limit an add evicts one, so
 * their number moves with the test order). */
typedef struct {
    int64_t kmalloc_bytes;
    int64_t slab_objects;
    int64_t pages;
    int64_t dentries;
} VfsEnvUsage;
void vfs_env_usage(VfsEnvUsage *u);

/* 1 if nothing leaked from a to b: the same kmalloc bytes and pages, and the
 * same slab objects apart from dentry-cache entries. Otherwise prints the
 * difference and returns 0. */
int vfs_env_no_leak(const VfsEnvUsage *a, const VfsEnvUsage *b);

/* Fail the n-th kmalloc / kmem_cache_alloc / alloc_pages from now (1 = the
 * next one); 0 turns it off. */
void vfs_env_fail_alloc_in(int n);

/* Nonzero: page allocations fail until a page is freed (memory ran out and
 * only reclaim can bring some back). */
extern int vfs_env_arena_full;

/* Called when the code under test would sleep on a wait queue. The hook may
 * change state (write to a pipe, close an end) and return; with no hook the
 * test fails (it would block forever). */
extern void (*vfs_env_sleep_hook)(void);
/* Called at the start of alloc_pages while set (a test clears it inside to run once) */
extern void (*vfs_env_alloc_hook)(void);
extern uint32_t vfs_env_last_gfp;   /* flags of the last alloc_pages call */
extern int vfs_env_sleeps;   /* wq_sleep calls so far */

/* Fake terminal: bytes written to it, and input that tty_read returns. */
extern char vfs_env_tty_out[4096];
extern size_t vfs_env_tty_out_len;
void vfs_env_tty_input(const char *s);

/* Uptime the VFS stamps times with (ms). */
extern uint64_t vfs_env_uptime_ms;

/* Read a whole file into buf (NUL-terminated). Returns bytes or -errno. */
int64_t vfs_env_read_file(const char *path, char *buf, size_t size);

/* A user-space address that faults in copy_to_user/copy_from_user. */
#define VFS_ENV_BAD_UADDR 0x10ull

/* User copies also fault from this host address on (0: off), so a buffer can
 * fault half-way. */
extern uintptr_t vfs_env_fault_at;

/* File-mapping tests (M1-12): the entry an address space has at virt (its
 * page and VMA protection). 1 if there is one. */
struct page;
int vfs_env_pte(uint64_t pgd, uint64_t virt, struct page **pg, uint32_t *flags);
/* Set an entry's accessed bit, as a CPU using it would */
void vfs_env_pte_touch(uint64_t pgd, uint64_t virt);
extern int vfs_env_tlb_flushes;   /* mm_flush_tlb_range and zap flushes so far */

#endif /* ARCHOS_TESTS_VFS_ENV_H */
