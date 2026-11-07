# ADR-0005: Async block layer with DMA

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
virtio-blk polls today. NVMe, AHCI and USB storage are interrupt-driven DMA devices; the page cache needs to issue
many requests at once.

## Decision
- A `blk_request` (op, 64-bit LBA, sector count, scatter-gather list of physical pages, completion callback, status)
  is submitted to a per-device queue; drivers complete it from interrupt context; waiters sleep on a wait queue.
- 64-bit LBAs and byte offsets everywhere. Devices report logical and physical sector size (512, 512e, 4Kn).
- Flush/FUA requests exist so `fsync` is real.
- Partitions (GPT, ADR-0008) appear as block devices layered on the disk.
- virtio-blk is moved onto this layer first.

## Consequences
- New storage drivers only implement submit + interrupt completion.
- Rules out: polling drivers and synchronous-only block APIs (a synchronous helper may wrap the async one).

## Verification
- QEMU test with many concurrent reads; host tests for request splitting and partition offset math.
