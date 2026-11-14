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

#endif /* ARCHOS_TESTS_BLK_MEM_H */
