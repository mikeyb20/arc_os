# Kernel locking

The lock order and the rules every kernel block follows (work-blocks.md §7), as the debug-build checker
(`kernel/proc/lockdep.c`) enforces them. Written by M1-05. When a block adds a lock, it picks a level from the
table below and adds a row; a new level goes in the gaps and in `kernel/proc/lockdep.h`.

## The order

A lock may be taken only while every lock already held has a **lower** level (outermost = lowest number). Two
locks of the same level may be held together only when the new one carries a subkey higher than every held lock
of that level (a lock without a subkey counts as 0). Give every lock of a nestable level a subkey.

| Level | Name in `lockdep.h` | Kind | Locks today | From |
|---|---|---|---|---|
| 0 | `LOCK_LEVEL_KERNEL` | big kernel lock | `kernel_lock` (see below) | M1-05, interim |
| 7 | `LOCK_LEVEL_FILE_POS` | sleeping | `File.pos_lock`: the position of a seekable open file (read, write, lseek, getdents) | M1-11 |
| 8 | `LOCK_LEVEL_RENAME` | sleeping | `rename_mutex` (vfs.c): one rename at a time, held from before either path is resolved | M1-11 |
| 10 | `LOCK_LEVEL_INODE` | sleeping, read-write | `Inode.lock` (`RwSem`): a directory's entries (lookup shared, changes exclusive), a regular file's data and size (read shared, write and truncate exclusive); a pipe's reader and writer mutexes | M1-11 |
| 11 | `LOCK_LEVEL_INODE_2` | sleeping, read-write | rename's second directory | M1-11 |
| 12 | `LOCK_LEVEL_INODE_CHILD` | sleeping, read-write | a directory being removed or replaced (rmdir, rename) | M1-11 |
| 20 | `LOCK_LEVEL_MM` | sleeping, read-write | `Mm.lock` (`RwSem`), per address space: VMA changes write, page faults and fork read | M1-07 |
| 30 | `LOCK_LEVEL_PAGE` | sleeping, per page | page lock | M1-12 |
| 40 | `LOCK_LEVEL_MAPPING` | sleeping | mapping-tree lock (VMAs mapping a file; reclaim, truncate) | M1-12 |
| 45 | `LOCK_LEVEL_FS_IO` | sleeping | a filesystem's device reads: the FAT32 volume's `io_lock` (cluster buffer, chain cursors) | M1-11 |
| 50 | `LOCK_LEVEL_PAGE_TABLE` | spinlock | `Mm.pt_lock`, per address space (inside `Mm.lock`); the kernel page tables (`vmm.c`) | M1-05, M1-07 |
| 60 | `LOCK_LEVEL_PAGE_CACHE` | spinlock | page-cache index lock, per address space | M1-12 |
| 62 | `LOCK_LEVEL_MOUNT` | spinlock | adding to the mount table (readers take no lock: entries are published, never removed) | M1-11 |
| 63 | `LOCK_LEVEL_DCACHE` | spinlock | the dentry cache: hash chains, LRU list, count | M1-11 |
| 64 | `LOCK_LEVEL_ICACHE` | spinlock | the inode cache: hash chains; a hashed inode's count goes 1 → 0 only under it | M1-11 |
| 65 | `LOCK_LEVEL_FD_TABLE` | spinlock | one fd table's slots and bitmaps | M1-11 |
| 70 | `LOCK_LEVEL_SIGNAL` | spinlock | thread-group and signal lock | M1-16 |
| 75 | `LOCK_LEVEL_OBJECT` | spinlock | one object's state, the lock handed to `wq_sleep`: pipe, socket, tty, wait4, mutex and semaphore guards | M1-05 |
| 76 | `LOCK_LEVEL_OBJECT_TABLE` | spinlock | a table looked up under an object lock: the socket table | M1-05 |
| 80 | `LOCK_LEVEL_WAITQUEUE` | spinlock | wait-queue locks | M1-05 |
| 90 | `LOCK_LEVEL_RUNQUEUE` | spinlock, interrupts off | per-CPU run queues; two at once only in CPU-index order (subkey = index + 1) | M1-05 |
| 99 | `LOCK_LEVEL_SLAB_REGISTRY` | spinlock | the list of slab caches | M1-05 |
| 100 | `LOCK_LEVEL_SLAB` | spinlock | one slab cache | M1-02b |
| 101 | `LOCK_LEVEL_BUDDY` | spinlock | buddy allocator (the only lock taken under a slab lock) | M1-02 |
| 109 | `LOCK_LEVEL_LOG` | spinlock | kernel log ring: `kprintf` works under any lock | M1-04 |

Levels 7, 8, 11, 12, 45 and 62–65 are M1-11's additions (see "VFS locks" below). Levels 75 and 76 are additions to §7's list: the pipe, socket and tty locks are the "condition" locks passed to
`wq_sleep`, so they sit just above the signal lock and below the wait-queue locks; the socket table is looked up
while one socket's lock is held. Timer-queue locks (M1-06) go between 101 and 109.

## Rules the checker enforces (debug builds, `ARC_DEBUG=1`)

Each rule panics with `LOCK ORDER VIOLATION`, the lock being taken and the locks held:

1. A spinlock is taken out of order (a lower level than one held, or the same level without a higher subkey).
   `spin_trylock` is recorded but not order-checked: it never waits, so it can't deadlock.
2. A sleeping lock (mutex) is taken out of order, or while any spinlock is held.
3. A thread enters `sched_schedule` (blocking or yielding) or waits on a semaphore while holding a spinlock.
4. The kernel lock is taken at syscall entry while any other lock is held. Taking it back after a sleep
   (`kernel_lock_resume`) only rules out spinlocks: a mutex may be held across the sleep, because anyone waiting
   for that mutex sleeps too, and sleeping gives the kernel lock up.
5. More than 16 spinlocks are held on one CPU, or more than 4 sleeping locks by one thread.

Release builds compile the checks away. QEMU suite `lockorder` takes a wait-queue-level lock and then a
page-table-level lock on purpose and expects the panic; host tests check `lockdep_check_order` itself.

## Rules the checker does not see

- Never take a page fault while holding a page lock: prefault the user buffer, copy with faults disabled, and
  retry on a short copy. The fault path never takes an inode lock.
- Any lock an interrupt handler takes is taken with `spin_lock_irqsave` everywhere else.
- The caller keeps the saved interrupt flags (`spin_lock_irqsave(lock, &flags)` /
  `spin_unlock_irqrestore(lock, flags)`), so locks may be released in any order; restore the outermost flags
  last. Plain `spin_lock` / `spin_unlock` only where interrupts are known to be off, or for a lock no interrupt
  handler takes.
- While a CPU holds any spinlock the slice timer does not preempt it (`hal_lock_depth`).

## The big kernel lock (interim)

Before M1-05 one CPU ran all kernel code with interrupts off, and the process list, PIDs, the VFS, ramfs, fd
tables and signal state relied on that instead of locks. `kernel_lock` (`kernel/proc/kernel_lock.h`) keeps that
guarantee while user code runs on every CPU:

- A thread holds it from syscall entry to syscall exit (and through `execve` until it enters user mode); a
  device interrupt handler (keyboard, virtio-net) holds it while it runs (`kernel_lock_irq`).
- It is held only with interrupts off. A thread that blocks gives it up inside `sched_schedule` and takes it
  back when it runs again, so sleeping with it is allowed.
- The timer, IPIs and the scheduler never take it. Wait queues and the scheduler are correct without it. Pipes,
  sockets and the tty have locks too, but parts of their state are still touched outside those locks (pipe data
  and reader/writer counts, the tty input buffer), so they stay under it until their row below is removed.

It goes away piece by piece as subsystems get their own locks. **BKL coverage** (ADR-0021): one row per
subsystem still under it. Only the lane that owns a subsystem edits its row; the block that moves the last one
off removes the lock.

| Subsystem still under the kernel lock | Lane | Taken off by |
|---|---|---|
| Process list, PID counter, fork/exec/exit/wait bookkeeping, credentials | KB | M1-16 |
| Signal state (`SigState`, delivery, stop and continue) | KB | M1-16 |
| Network stack outside the socket locks (interfaces, ARP, IP, UDP/TCP demux) | KB | M1-18 |
| TTY line discipline and VT/ANSI state (the keyboard IRQ holds the lock) | KB | M1-09 |

M1-11 took the VFS, ramfs, devfs, procfs, FAT32, path lookup, fd tables and pipes off this list: they are
correct without the kernel lock (it is still taken at every syscall entry for the rows above). What they read
from other subsystems stays covered by those rows: a process's cwd, umask and credentials, and procfs's walks of
the process list (the process row, M1-16); the tty line discipline (the TTY row).

## VFS locks (M1-11)

- Order, outermost first: `File.pos_lock` (7) → `rename_mutex` (8) → directory or file `Inode.lock`
  (10, then 11 and 12 in rename and rmdir) → `Mm.lock` (20, a page fault in a user copy) → FAT32 `io_lock` (45)
  → the VFS spinlocks (62–65), which are leaves apart from the slab allocator.
- User memory may be touched under the sleeping VFS locks (a fault takes only mm locks, which come after them),
  never under the spinlocks. FAT32 copies to the user outside `io_lock` (it reads a cluster into a kernel buffer
  first), so `io_lock` can later sit under a page lock (30) in M1-12's `readpage`.
- Path lookup holds one directory lock at a time, shared; a component found in the dentry cache takes no
  directory lock at all. Directory changes take the parent exclusive and remove the affected dentry before the
  filesystem changes anything, so lookups never cache a name being removed.
- Rename takes `rename_mutex` before it resolves either path. Only rename moves a directory, so the resolved
  paths stay true while it is held, and their text gives the exact ancestry (one mount, no directory hard
  links): the ancestor parent is locked first (10, then 11), then a replaced directory (12), and a directory
  can't be moved into itself. Resolving first and locking after would let another rename change the tree in
  between (a detached cycle, or an ABBA with rmdir that the level checks can't see).
- `iput` that frees an inode runs its filesystem's `evict_inode` with no lock held; `fput` never runs under a
  spinlock. Pipes: `rd_mutex` and `wr_mutex` (level 10) serialise readers and writers and are held across user
  copies but dropped while waiting; `pipe->lock` (75) guards head, tail and the counts and is the wait-queue
  condition lock. A release wakes the other side before dropping `pipe->lock` (after it, the other end's last
  release may free the pipe).
- Directory changes re-check `I_DEAD` under the parent's lock (create, mkdir, rename's target), since the
  directory may be removed between resolving and locking it.

## Address-space locks (M1-07)

Every user address space (`struct Mm`, `kernel/mm/mm.h`) has two locks, and the mm needs no kernel lock: user
page faults run without it.

- `Mm.lock`, a sleeping read-write lock. Writers: mmap, munmap, mprotect, madvise, mremap, brk, stack growth
  (the fault drops its read hold and takes the write hold), the OOM reaper. Readers: page faults, fork's copy,
  `/proc/<pid>/maps`. A waiting writer holds new readers back. Not recursive.
- `Mm.pt_lock`, a spinlock taken inside `Mm.lock` around every change to that mm's page tables. Nothing sleeps
  or allocates a user page under it (a page-table page may be allocated: that never sleeps).
- **Never touch user memory while holding either.** The fault would take `Mm.lock` again (a deadlock behind a
  waiting writer). Copy user data into a kernel buffer first, then lock.
- A kernel fault on a user address (inside `copy_from_user` and friends) with a spinlock held is not resolved:
  the copy fails with `-EFAULT`. So user copies happen with no spinlock held.
- TLB: unmapped pages and emptied page tables go on a list; the range is flushed on every CPU in `Mm.cpus`
  after `pt_lock` is released and before anything on the list is freed. `tlb_shootdown` must be called with no
  spinlock held; a CPU spinning with interrupts off for another CPU's lock answers shootdowns by hand
  (`kernel_lock_irq`, `tlb_shootdown_poll`).
- `Mm.users` (atomic) keeps an mm alive for a holder that may sleep while it reads another process's mm (OOM
  reaper, `/proc/<pid>/maps`). The `Process.mm` pointer itself changes only in exec and exit, which still run
  under the kernel lock (the process row above, M1-16).

## Scheduler locks (M1-05)

- A CPU switches threads only while holding its own run-queue lock. The lock is carried across
  `context_switch` and released by the thread switched to (`sched_finish_switch`).
- A wakeup takes the target thread's run-queue lock (under the wait-queue lock when it comes from `wq_wake`).
- An idle CPU pulling work locks its own queue and the busiest one, in CPU-index order.
- No CPU changes a thread another CPU is running: an affinity change moves a running thread at its next
  schedule.
