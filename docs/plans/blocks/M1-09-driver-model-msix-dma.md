# M1-09 — Driver model, MSI-X and DMA helpers

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M1-05 | M1-10, all M4 drivers |

**Goal.** Drivers register with a bus and bind through ID tables, interrupts use MSI or MSI-X when the device
supports them, and DMA memory comes from one helper (ADR-0006, ADR-0005).

**Read first.**
- `docs/decisions/0006-driver-model.md`, `0005-async-block-layer.md`
- `kernel/drivers/pci.c`, `pci.h`, `virtio.c`, `virtio.h`, `virtio_blk.c`, `virtio_net.c`
- `kernel/arch/x86_64/ioapic.c`, `idt.c`, `lapic.c`

**Build.**
1. `struct bus`, `struct device`, `struct driver` with `probe`, `remove`, `suspend`, `resume` and a match table.
   The PCI bus enumerates and matches by vendor, device, subsystem IDs or class with mask.
2. IRQ layer: per-CPU vector allocator; `pci_alloc_irq_vectors(dev, min, max, types)` then
   `irq_request(dev, index, handler, arg, flags, name)` (built as the two-step Linux shape: the bus hands out
   descriptors, the driver attaches handlers by index); MSI-X table setup (BAR mapping, mask entries before
   programming), MSI fallback, IOAPIC INTx fallback; interrupt affinity to a CPU (only CPUs with APIC ID below 256
   as MSI targets; no interrupt remapping).
3. MMIO helper: uncached mappings of BARs (never unmapped for now).
4. DMA helper: `dma_alloc_coherent(dev, size, &bus_addr, flags)` (contiguous from the buddy allocator, optional
   below-4 GiB) and a streaming map/unmap pair. On x86 without an IOMMU these are identity mappings, but every
   driver goes through them.
5. Move virtio-blk and virtio-net onto the model with MSI-X. `suspend`/`resume` exist and log "not supported yet".
6. `/proc/interrupts` shows MSI-X vectors per CPU; a PCI device list with bound drivers (the boot report reuses it).
7. Tests: QEMU virtio devices with MSI-X; the disk and network suites still pass; host tests for ID matching.

**Done when.**
- [x] Suites pass; no driver touches PCI config space or IRQ routing outside the bus and IRQ layers.
- [x] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
  `arch/x86_64/`, and call the HAL through `hal_*` functions, not x86 names (work-blocks.md §7); the
  `arch_includes_outside_arch` limit in the budgets file is lowered to match (ADR-0020).

**Watch out.**
- MSI address/data differs between xAPIC and x2APIC destinations. Keep it inside the IRQ layer.
- `suspend`/`resume` signatures are part of ADR-0006; get them right now even though they do nothing yet.

**Notes.**
- From M1-03: `lapic_x2apic()` says which mode runs; I/O APIC routes take an 8-bit APIC ID (`ioapic_route_isa`
  warns and skips routing if the BSP's ID is above 255). MSI destinations above 255 need interrupt remapping (out
  of scope, as the spec says).
- From M1-05: device interrupt handlers (keyboard, virtio-net) take the big kernel lock with `kernel_lock_irq`
  around the code that touches unlocked kernel state (tty, network stack). Drivers on the new bus model should
  keep their own IRQ-safe locks (levels in `docs/kernel/locking.md`) so they can drop it.

**Handoff (M1-09, built 2026-11-13).**
- *Shape.* `drivers/bus.{h,c}` is the generic model (Bus, Device, Driver; probe, remove, suspend(dev, how),
  resume(dev, how) with `PM_SYSTEM` / `PM_RUNTIME`); `drivers/pci.c` is the PCI bus (ID tables with subsystem IDs
  and class masks, BAR sizing, capability walk, config access under an IRQ-safe lock, 16-bit accesses through the
  matching half of the data port). `drivers/irq.{h,c}` is the arch-neutral IRQ layer (descriptors, handlers,
  CPU choice, affinity, INTx line chains, counts, `/proc/interrupts`); `drivers/pci_irq.c` programs MSI-X, MSI and
  INTx for it through `IrqChip` ops; `arch/x86_64/irq_vector.c` owns vectors 0x30-0xDF per CPU and the MSI
  address/data format, reached only through `hal_irq_*`. `drivers/mmio.c` maps BARs uncached into a 512 MiB
  window at `HAL_MMIO_BASE` (bump allocator, never unmapped); `drivers/dma.c` is the DMA helper (identity on x86,
  mask checks, no bounce buffers).
- *For M1-10.* virtio-blk binds through the bus with MSI-X (entry 0 config, entry 1 queue) or one shared INTx
  handler, but still **polls** for completion: the queue interrupt only counts. M1-10 replaces the poll with
  interrupt completion. Two latent bugs it must fix while there: a request that hits `POLL_TIMEOUT` frees DMA
  buffers the device may still write (virtio-blk and virtio-net TX), and `POLL_TIMEOUT` is a loop count, not time.
  Request buffers come from `dma_alloc_coherent`; use `dma_map_page` for page-cache pages. Keep a time-bounded
  completion fallback: an interrupt that never comes (INTx on real hardware, below) must fail the request, not hang
  it.
- *virtio-net.* The RX interrupt only wakes a kernel worker thread (`net_rx_worker`), which takes the kernel lock
  like a syscall and polls the ring; the interrupt no longer takes the kernel lock, and nothing allocates in hard-IRQ
  context (work-blocks §7). TX has no vector (`VRING_AVAIL_F_NO_INTERRUPT`). `rx_kicks` / `rx_wakeups` count both.
- *Locks.* New levels: 5 device (sleeping, around binding and probe/remove/suspend), 95 IRQ descriptors, 96
  vector tables, 97 PCI config (all IRQ-safe spinlocks, never taken in an interrupt). Level 5 sits between the
  kernel lock and lane KA's level 1, outside both lanes' ranges, because probe sleeps, allocates and maps MMIO
  (page-table lock): a runtime resume that would run from an I/O path holding a KA lock must not take it (M4-02).
  The TTY row of the BKL table moved to M1-18 (Michael, 2026-11-13).
- *INTx.* Legacy lines come from the firmware's `irq_line` (no ACPI `_PRT` until uACPI, M4-11). In APIC mode the I/O
  APIC already routes lines 0-15 with the MADT's trigger modes; QEMU routes PCI INTx there level-triggered, so lines
  a PCI device uses get their EOI after the handler (`isr_set_late_eoi`), else the line fires again at once. On real
  chipsets this is likely **wrong**, not just unproven: `irq_line` is the 8259 PIRQ routing, and in APIC mode a PCH
  delivers PCI INTx on I/O APIC pins 16-23 chosen by `_PRT`. So `pci_alloc_irq_vectors` prints a loud warning for
  INTx in APIC mode (`hal_irq_line_routing_known`); M4-11 routes it properly. Every device in plan scope has MSI or
  MSI-X. Also pre-existing: a level line nobody claims storms forever (no "nobody cared" disable).
- *MSI without per-vector masking* can't be masked safely (turning MSI off loses an event raised meanwhile), so its
  chip is `IRQCHIP_FIXED_TARGET`: `irq_set_affinity` refuses it, and Enable is toggled only at request and free. AHCI
  on Intel PCHs is usually plain MSI without masking, so it keeps the CPU it was given. edu is the same in QEMU; the
  MSI-X move is tested on virtio-net's RX vector instead.
- *Concurrency.* A handler can run on two CPUs at once right after a move (old vector still in flight); `irq.h`
  says so, and handlers keep their own IRQ-safe lock (virtio-net's `rx_lock`).
- *Power order.* `bus_suspend_all` runs children before parents (newest bus, newest device first; a bus adds a
  parent before what sits behind it), resume the reverse. `Device.parent` exists and is NULL everywhere until a bus
  with devices behind another device (USB behind xHCI, M4-02) sets it.
- *What is not done yet.* `irq_desc_destroy` keeps a descriptor that ever had a handler (listed as "(free)"),
  because nothing waits for a handler still running on another CPU; hot-unplug (M4-02) adds that wait. Suspend
  and resume are stubs returning `-EOPNOTSUPP`, so `bus_suspend_all` refuses and resumes (ADR-0017 work, M4).
  Review nits left for later: `irq_request` doesn't try another CPU when the chosen one's vectors run out
  (`-ENOSPC`, needs many retired vectors); `irq_line_entry` reads the x86 `InterruptFrame` vector (the HAL should
  pass the line number before a second architecture); an MSI-X allocation that fails with `-ENOMEM` doesn't fall
  back to MSI (no memory for MSI either); virtio probe error paths don't reset the device (fine at boot; with
  `remove`).
- *Smoke boot.* The boot report lists each PCI function with its bound driver and interrupts, then one line per
  MSI-X table (`MSI-X bb:dd.f N entries in BARb+0xoff (K KiB), M masked`), read and never driven; that line is the
  hardware proof for this block. Tables are read only where memory decoding is already on.
- *Tests.* Host: `bus`, `irq`, `irq_vector`, `dma`, `pci` (fake config space with BAR sizing, MSI/MSI-X
  programming), plus `isr` (device vectors, late EOI), `procfs`, `virtio`, `vmm` (uncached flag). QEMU:
  `qemu.devices` (virtio-blk and virtio-net on MSI-X, edu on MSI whose move is refused, a DMA round trip, and an ARP
  round trip with virtio-net's RX vector moved to another CPU first) and `qemu.devices.intx` (virtio-blk
  `vectors=0`, edu with `arc.devtest=intx`, the INTx warning). Checks that cross CPUs wait on what the other CPU
  writes last (the handler's CPU, or the per-CPU count itself): the IRQ layer counts after the handler returns, and
  under KVM the waiting CPU sees the handler's effect first (TCG hid this; CI caught it).
  `tools/qemu-test.py --qemu-arg` passes extra devices. `kernel/diag/devtest.c` (test mode only) drives edu.
- *QEMU quirk.* edu cannot DMA above 4 GiB in QEMU 8.2: a 64-bit write of its address registers is split and the
  upper half dropped, so the suite gives edu a 32-bit mask and the driver says so.
- *Found, not fixed (lane KA).* FAT32 never frees a node: `fat32_alloc_node` caches nodes by first cluster in a
  256-entry table, so each distinct non-empty file costs one 96-byte `VfsNode` forever, and past 256 files (or for
  empty files, cluster 0) every lookup allocates. Reading `/disk/hello.txt` from a suite shows
  `heap_leaked_bytes 96`, so the devices suite reads no file. The cache lookup is also a linear scan. Node lifetime
  and the inode cache belong to M1-11.
- *Cross-lane touches.* `mm/vmm.{h,c}`: one flag, `VMM_FLAG_UNCACHED` (PCD|PWT) at bit 4 (M1-07 takes bit 3 for
  `VMM_FLAG_PROTNONE`; the flags block has an assert that the bits are distinct), and its read-back;
  `tests/test_vmm.c` tests it. `mm/page.h` / `mm/kmalloc.c`: `page_order_for` moved there. `arch/x86_64/smp.c`: an AP loads the kernel page tables first thing, so a device
  interrupt that reaches an idle AP before the scheduler starts sees the MMIO window.
