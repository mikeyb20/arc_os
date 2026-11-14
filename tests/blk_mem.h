#ifndef ARCHOS_TESTS_BLK_MEM_H
#define ARCHOS_TESTS_BLK_MEM_H

/* The block layer over memory for the VFS host suites (blk_mem.c). */

#include "drivers/blkdev.h"

extern int blk_mem_fail;       /* nonzero: every request fails with -EIO */
extern int blk_mem_requests;   /* requests that reached the "device" */
extern int blk_mem_flushes;    /* BLK_OP_FLUSH requests (not in blk_mem_requests) */
extern int blk_mem_defer;      /* nonzero: requests wait for blk_mem_run */

#define BLK_MEM_POISON 0xAA   /* what a deferred read writes */

/* The device list blk_get (and so the blk_find_* lookups) walks: n devices
 * at devs, kept by the caller (n = 0: none). */
void blk_mem_devices(BlockDevice **devs, unsigned n);

/* End every deferred request (reads fill BLK_MEM_POISON), as interrupts would. */
void blk_mem_run(void);

/* Write-order audit (M1-15). With blk_mem_classify set, every write is put
 * in a class by the test (BLK_MEM_DATA for file pages, a higher class, or
 * < 0 for no class) and counted per device as in flight, then done but not
 * yet flushed (only on a BLK_CACHE device), then flushed (BLK_OP_FLUSH
 * clears what had completed before it). While blk_mem_audit is set, a write of class c is an error if any
 * class blk_mem_before[c] names (a bit mask) has writes in flight or not
 * flushed: what a crash right then could leave out of order. The first
 * error is kept in blk_mem_order_error. */
#define BLK_MEM_DATA    0
#define BLK_MEM_CLASSES 4
extern int (*blk_mem_classify)(const BlockDevice *dev, uint64_t sector, int file_page);
extern unsigned blk_mem_before[BLK_MEM_CLASSES];
extern int blk_mem_audit;
extern int blk_mem_order_errors;
extern char blk_mem_order_error[200];

#endif /* ARCHOS_TESTS_BLK_MEM_H */
