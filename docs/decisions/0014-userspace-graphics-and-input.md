# ADR-0014: Graphics and input from user space

- **Status:** Accepted
- **Date:** 2026-11-07

## Decision
- The firmware framebuffer is exposed as `/dev/fb0` with Linux fbdev ioctls (`FBIOGET_VSCREENINFO`,
  `FBIOGET_FSCREENINFO`) and `mmap`.
- A program can take the framebuffer from the text console (and give it back) through a VT ioctl.
- Input comes from `/dev/input/eventN` (ADR-0007).

## Consequences
- The image viewer, map viewer and any future GUI are ordinary user programs. Rules out: graphics code in the kernel
  beyond the text console.

## Verification
- QEMU test: a user program draws a test pattern via `/dev/fb0` and returns to the console.
