#ifndef ARCHOS_PROC_KERNEL_LOCK_H
#define ARCHOS_PROC_KERNEL_LOCK_H

/* The big kernel lock (M1-05, interim).
 *
 * Before M1-05 one CPU ran all kernel code, and "interrupts off" was the only
 * thing that kept two syscalls apart: the process list, PIDs, the VFS, ramfs,
 * fd tables and signal state have no locks of their own. The kernel lock keeps
 * that guarantee now that every CPU runs threads: user code runs in parallel,
 * kernel code that touches those structures runs on one CPU at a time.
 *
 * Who holds it: a thread from syscall entry to syscall exit (and through
 * execve until it enters user mode); a device interrupt handler while it runs.
 * It is held only with interrupts off, so no interrupt handler can find its
 * own CPU holding it. A thread that sleeps or blocks gives it up inside
 * sched_schedule() and takes it back when it runs again, so sleeping with it is
 * allowed. The timer, IPIs and the scheduler never take it.
 *
 * Level 0: taken before every other lock, never while holding one. Later
 * blocks give subsystems their own locks and shrink what it covers (M1-07 mm,
 * M1-11 VFS, M1-16 thread groups); docs/kernel/locking.md lists what it still
 * covers. */

/* Take the kernel lock from thread context with no other lock held (syscall
 * entry, a thread taking it back after sleeping). Interrupts must be off; they
 * are let in briefly while it spins, so IPIs and the timer still reach this
 * CPU. */
void kernel_lock(void);

/* Take the kernel lock in a device interrupt handler (spins with interrupts off). */
void kernel_lock_irq(void);

/* Release the kernel lock. */
void kernel_unlock(void);

/* 1 if the running thread holds the kernel lock. */
int kernel_lock_held(void);

/* Scheduler only: give the lock up for a thread that is switching away while
 * holding it. Returns 1 if it was held (pass to kernel_lock_resume). */
int kernel_lock_suspend(void);

/* Scheduler only: take the lock back for a thread that held it when it
 * switched away (was = kernel_lock_suspend's result). */
void kernel_lock_resume(int was);

#endif /* ARCHOS_PROC_KERNEL_LOCK_H */
