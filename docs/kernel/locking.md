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
| 5 | `LOCK_LEVEL_DEVICE` | sleeping | device model: bus device and driver lists, binding, probe/remove/suspend/resume calls (`drivers/bus.c`) | M1-09 |
| 10 | `LOCK_LEVEL_INODE` | sleeping | inode lock: write, truncate, rename, directory changes | M1-11 |
| 20 | `LOCK_LEVEL_MM` | sleeping, read-write | mm lock: mmap, munmap, mprotect, page faults | M1-07 |
| 30 | `LOCK_LEVEL_PAGE` | sleeping, per page | page lock | M1-12 |
| 40 | `LOCK_LEVEL_MAPPING` | sleeping | mapping-tree lock (VMAs mapping a file; reclaim, truncate) | M1-12 |
| 50 | `LOCK_LEVEL_PAGE_TABLE` | spinlock | page-table lock, per mm; today the kernel page tables (`vmm.c`) | M1-05 |
| 60 | `LOCK_LEVEL_PAGE_CACHE` | spinlock | page-cache index lock, per address space | M1-12 |
| 70 | `LOCK_LEVEL_SIGNAL` | spinlock | thread-group and signal lock | M1-16 |
| 75 | `LOCK_LEVEL_OBJECT` | spinlock | one object's state, the lock handed to `wq_sleep`: pipe, socket, tty, wait4, mutex and semaphore guards | M1-05 |
| 76 | `LOCK_LEVEL_OBJECT_TABLE` | spinlock | a table looked up under an object lock: the socket table | M1-05 |
| 80 | `LOCK_LEVEL_WAITQUEUE` | spinlock | wait-queue locks | M1-05 |
| 90 | `LOCK_LEVEL_RUNQUEUE` | spinlock, interrupts off | per-CPU run queues; two at once only in CPU-index order (subkey = index + 1) | M1-05 |
| 95 | `LOCK_LEVEL_IRQ_DESC` | spinlock, IRQ-safe | interrupt descriptors: request, free, affinity, INTx line chains, `/proc/interrupts` (`drivers/irq.c`); never taken in an interrupt | M1-09 |
| 96 | `LOCK_LEVEL_IRQ_VECTOR` | spinlock, IRQ-safe | per-CPU device vector tables (`arch/x86_64/irq_vector.c`), under the descriptor lock; never taken in an interrupt | M1-09 |
| 97 | `LOCK_LEVEL_PCI_CONFIG` | spinlock, IRQ-safe | PCI config ports 0xCF8/0xCFC (`drivers/pci.c`): one address-then-data pair at a time; leaf | M1-09 |
| 99 | `LOCK_LEVEL_SLAB_REGISTRY` | spinlock | the list of slab caches | M1-05 |
| 100 | `LOCK_LEVEL_SLAB` | spinlock | one slab cache | M1-02b |
| 101 | `LOCK_LEVEL_BUDDY` | spinlock | buddy allocator (the only lock taken under a slab lock) | M1-02 |
| 109 | `LOCK_LEVEL_LOG` | spinlock | kernel log ring: `kprintf` works under any lock | M1-04 |

Levels 75 and 76 are additions to §7's list: the pipe, socket and tty locks are the "condition" locks passed to
`wq_sleep`, so they sit just above the signal lock and below the wait-queue locks; the socket table is looked up
while one socket's lock is held. Timer-queue locks (M1-06) go between 101 and 109.

M1-09 adds the device levels. Probe and remove run under the device lock (5, sleeping), so a driver may sleep,
allocate and take any lock from a later row from them, but never hold a spinlock across them. The descriptor
lock (95) nests the vector-table lock (96) and the PCI config lock (97), because masking and retargeting an
interrupt writes config space; it also allocates under itself (the first vector table of a CPU: slab and buddy,
99-101, still above it). Interrupt handlers take none of the three: a device vector is one table load, an INTx
line walks its chain without a lock (it only grows at the head, and an unlinked entry keeps its next pointer).

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

- A thread holds it from syscall entry to syscall exit (and through `execve` until it enters user mode); the
  keyboard interrupt handler holds it while it runs (`kernel_lock_irq`). Since M1-09 virtio-net's interrupt does
  not: it only wakes the RX worker thread, which takes the lock like a syscall (interrupts off, no other lock).
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
| mm: user address spaces, `brk`, page-table setup in exec and fork | KA | M1-07 |
| VFS, ramfs, devfs, procfs, FAT32, path lookup | KA | M1-11 |
| fd tables (open, close, dup, fork copy) | KA | M1-11 |
| Pipe buffers and reader/writer counts (`pipe.c` moves bytes and checks counts outside `pipe->lock`) | KA | M1-11 |
| Process list, PID counter, fork/exec/exit/wait bookkeeping, credentials | KB | M1-16 |
| Signal state (`SigState`, delivery, stop and continue) | KB | M1-16 |
| Network stack outside the socket locks (interfaces, ARP, IP, UDP/TCP demux) | KB | M1-18 |
| TTY line discipline and VT/ANSI state (the keyboard IRQ holds the lock) | KB | M1-18 |

## Scheduler locks (M1-05)

- A CPU switches threads only while holding its own run-queue lock. The lock is carried across
  `context_switch` and released by the thread switched to (`sched_finish_switch`).
- A wakeup takes the target thread's run-queue lock (under the wait-queue lock when it comes from `wq_wake`).
- An idle CPU pulling work locks its own queue and the busiest one, in CPU-index order.
- No CPU changes a thread another CPU is running: an affinity change moves a running thread at its next
  schedule.
