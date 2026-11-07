# ADR-0006: Driver model and MSI-X

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
M4 adds xHCI, NVMe, AHCI, e1000e, RTL8111 and more. Without a model, each driver edits the kernel in several places.

## Decision
- Buses (PCI first, USB second) enumerate devices and match registered drivers by vendor/device ID or class code.
- A driver provides `probe`, `remove`, `suspend`, `resume` (ADR-0017) and a match table, and registers with the subsystem it serves
  (block, net, input, fb).
- PCIe devices use MSI-X (or MSI) when available; legacy INTx via IOAPIC is a fallback.
- DMA memory comes from a helper that returns physically contiguous, cache-coherent buffers with known physical
  addresses.
- Each driver lives in its own file(s) under `kernel/drivers/`; nothing outside the driver knows its registers.

## Consequences
- Adding a driver is one directory plus a match-table entry.
- Rules out: hard-coded device setup in `kmain`.

## Verification
- Existing virtio drivers ported to the model; QEMU tests with `-device nvme`, `ahci`, `e1000e`, `qemu-xhci`.
