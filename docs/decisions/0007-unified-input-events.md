# ADR-0007: Unified input events

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
Keyboards and mice arrive via PS/2 today and via USB HID later. The TTY reads scancodes directly.

## Decision
- Every input driver emits Linux-compatible `struct input_event` (`time`, `type`, `code`, `value`) using Linux
  `EV_*`/`KEY_*`/`REL_*`/`BTN_*` codes.
- Devices appear as `/dev/input/eventN`. The TTY/console is just another consumer that turns key events into
  characters through a keymap.
- PS/2 keyboard is converted first; PS/2 mouse and USB HID follow.

## Consequences
- USB keyboards and mice work everywhere the PS/2 ones do, without TTY changes.
- Rules out: drivers that write characters straight into the TTY.

## Verification
- Host tests for scancode → keycode; QEMU test that reads events from `/dev/input/event0`.
