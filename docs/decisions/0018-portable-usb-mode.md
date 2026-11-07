# ADR-0018: Portable mode — arc_os runs entirely from its USB key

- **Status:** Accepted (requested by Michael, 2026-11-07; built across M1, M4 and M5)
- **Date:** 2026-11-07

## Context
The low-tier test machine is a former work computer whose internal disk must stay as it is: arc_os can only run
from a USB key, the way Tails does. Many machines other people own will be the same. Michael wants this designed in
now, built later, with more drivers added over time so the key works on more machines.

Done late, portable use would need a second image format, a second boot path and rules about the host's disks
added after the fact. Decided now, it costs a few small items in blocks that are already planned.

## Decision
- **The key is an ordinary arc_os disk.** A portable key has the same GPT layout as an internal install
  (ADR-0008): ESP (FAT32: Limine, kernel, initramfs), system (ext2, read-only at runtime), data (ext2: `/apps`,
  `/models`, `/packs`, `/home`, `/var`, ADR-0009). Running from the key and from an internal disk is the same code
  path. Installing means writing this layout to another disk. There is no separate live-image format.
- **Boot disk identity.** Limine reports the GPT disk GUID and partition GUID it loaded the kernel from. `BootInfo`
  carries them, and the kernel mounts system and data from that same disk. When it can't (for example a boot from
  an ISO), it falls back to partition labels.
- **Partition types.** arc_os partitions have their own GPT type GUIDs and labels:

  | Partition | Type GUID | Label |
  |---|---|---|
  | system | `0BD6D742-55DB-4D77-8646-D17A35F4BE85` | `arc-system` |
  | data | `AFFB8E56-A8BE-4817-9A15-F945F7238591` | `arc-data` |

  Linux tools still read them as ext2 (ADR-0008's "readable on any Linux machine" holds); Windows doesn't mount
  partition types it doesn't know.
- **Hands off the host's disks.** arc_os mounts only partitions with its own type GUIDs, from the boot disk unless
  the user names another arc_os disk. It may read other disks' partition tables to identify them, and never writes
  to a disk it didn't create, except when the user runs the installer and confirms the target by model and size.
- **RAM mode.** Kernel, initramfs and optionally a small pack are loaded into RAM by Limine through the firmware,
  so arc_os boots on machines whose USB controller it doesn't drive yet. It serves hardware bring-up and recovery.
  Full packs and models need the key's storage driver (xHCI + USB mass storage, M4).
- **Flash-friendly and unplug-safe.** The system partition is mounted read-only; no access-time updates on arc_os
  partitions; writeback stays deferred and batched (ADR-0017). After an unclean shutdown the data partition is
  checked with `e2fsck` at the next boot (from M5-01; before that, it is mounted with a warning). Pulling the key
  gives running programs I/O errors and shows a message; it never crashes the kernel.
- **Secure Boot** stays off (knowledge-os.md §3.1). A machine that can't turn it off needs a Microsoft-signed shim
  plus our own enrolled key (MOK) in front of Limine. That is a separate decision, taken when such a machine has to
  be supported.
- **Hardware reach.** On an unknown machine the key depends on xHCI and USB mass storage (M4-01, M4-02, M4-04),
  which are essential, not optional. Wider compatibility (USB hubs, more network chips, AMD power management,
  chipset quirks) is a later compatibility track after M5, built for machines Michael chooses to support.

## Consequences
- One image format serves install and portable use. The M5 key builder writes the portable layout straight onto
  the stick, sized to fill it; the installer copies the same layout to an internal disk where a machine allows it.
- Speed depends on the stick. USB 3 with good random reads (or a USB SSD) is recommended; USB 2 works but loads
  models and searches more slowly. Measured in M4-14.
- Keys given to other people carry content and models, so their licences must allow redistribution (manifests
  already record licences, ADR-0010, ADR-0011).
- Rules out: a separate live-image format (squashfs + overlay), writing to host disks outside the installer, and
  depending on the internal disk for anything. Encrypting the data partition is not planned; it would need its own
  ADR.

## Verification
- QEMU: boot from a USB-attached image (`-device qemu-xhci -device usb-storage`) with a second disk carrying another
  OS's partitions; after a full test run, the second disk's bytes are unchanged (hash before and after).
- QEMU: remove the key mid-run (`device_del`); programs get I/O errors, the kernel keeps running.
- Real hardware (M4-14, M5-06): the USB-only machine runs the M3 test from the key; its internal disk is never
  mounted, and where it is readable, raw-sector hashes match before and after.
