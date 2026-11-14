#ifndef ARCHOS_TESTS_BLK_MEM_H
#define ARCHOS_TESTS_BLK_MEM_H

/* The block layer over memory for the VFS host suites (blk_mem.c). */

extern int blk_mem_fail;       /* nonzero: every request fails with -EIO */
extern int blk_mem_requests;   /* requests that reached the "device" */
extern int blk_mem_defer;      /* nonzero: requests wait for blk_mem_run */

#define BLK_MEM_POISON 0xAA   /* what a deferred read writes */

/* End every deferred request (reads fill BLK_MEM_POISON), as interrupts would. */
void blk_mem_run(void);

#endif /* ARCHOS_TESTS_BLK_MEM_H */
