---
name: driver-bringup
description: Bring up a new arc_os hardware driver (PCI or USB) through the driver model, tested in QEMU first and then on the real test machines. Use for NVMe, AHCI, xHCI, USB HID/storage, e1000e, RTL8111 and similar.
---

# driver-bringup

Drivers follow ADR-0006 (driver model), ADR-0005 (async block layer), ADR-0007 (input events) and ADR-0014 (fb).

## 1. Identify the hardware
- Vendor/device IDs and class codes from the test machines (`lspci -nn` on Linux) and from QEMU.
  Test machines are listed in `docs/plans/knowledge-os.md` §4.
- The authoritative spec (NVMe base spec, AHCI 1.3.1, xHCI 1.2, USB HID 1.11, Intel 8257x/I219 datasheet, Realtek
  datasheet). Record section numbers for every register you use in comments.

## 2. Plan the files
- `kernel/drivers/<name>/` with `<name>.c`, `<name>.h` (public API only) and `<name>_regs.h` (register offsets
  and bit fields as named constants; no magic numbers in `.c`).
- Write the header first: match table, `probe`/`remove`, the subsystem interface it implements.

## 3. Implement in this order
1. Match + probe: map BARs, read version/capabilities, log them.
2. Reset and initialize the controller; time out every wait (no infinite polling loops).
3. DMA buffers from the DMA helper (physically contiguous, known physical address).
4. Interrupts: MSI-X, then MSI, then INTx fallback.
5. Plug into the subsystem: block (submit + completion), net (tx/rx rings), input (`input_event`), fb.
6. `remove` / shutdown path that quiesces DMA.

## 4. Test
- QEMU device first: `-device nvme`, `-device ahci` + `ide-hd`, `-device qemu-xhci` + `usb-kbd`/`usb-mouse`/`usb-storage`,
  `-device e1000e`. Add a QEMU integration test that exercises the full path (read/write blocks, receive a key,
  ping, ...).
- Host tests for pure logic: descriptor/ring math, register field encoding, parsers.
- Real hardware: boot from USB on the test machine; logs go to the framebuffer and the log file (the laptop has no
  serial port). Record results and any quirks in the PR.

## 5. Rules
- Never block in interrupt context; complete requests and wake waiters.
- No driver-specific code outside its directory except the match-table registration.
- Unknown hardware revisions: log and refuse to bind rather than guess.

## Done when
QEMU test passes, the driver binds and works on the real test machine that has the device, and the plan's M4
checklist item is ticked.
