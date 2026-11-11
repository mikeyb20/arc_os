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
2. IRQ layer: per-CPU vector allocator; `request_irq(dev, handler, flags)`; MSI-X table setup (BAR mapping, mask
   entries before programming), MSI fallback, IOAPIC INTx fallback; interrupt affinity to a CPU (only CPUs with
   APIC ID below 256 as MSI targets; no interrupt remapping).
3. MMIO helper: uncached mappings of BARs (never unmapped for now).
4. DMA helper: `dma_alloc_coherent(dev, size, &bus_addr, flags)` (contiguous from the buddy allocator, optional
   below-4 GiB) and a streaming map/unmap pair. On x86 without an IOMMU these are identity mappings, but every
   driver goes through them.
5. Move virtio-blk and virtio-net onto the model with MSI-X. `suspend`/`resume` exist and log "not supported yet".
6. `/proc/interrupts` shows MSI-X vectors per CPU; a PCI device list with bound drivers (the boot report reuses it).
7. Tests: QEMU virtio devices with MSI-X; the disk and network suites still pass; host tests for ID matching.

**Done when.**
- [ ] Suites pass; no driver touches PCI config space or IRQ routing outside the bus and IRQ layers.
- [ ] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
  `arch/x86_64/`, and call the HAL through `hal_*` functions, not x86 names (work-blocks.md §7); the
  `arch_includes_outside_arch` limit in the budgets file is lowered to match (ADR-0020).

**Watch out.**
- MSI address/data differs between xAPIC and x2APIC destinations. Keep it inside the IRQ layer.
- `suspend`/`resume` signatures are part of ADR-0006; get them right now even though they do nothing yet.

**Notes.**
-
