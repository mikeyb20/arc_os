#ifndef ARCHOS_ARCH_X86_64_DEBUG_EXIT_H
#define ARCHOS_ARCH_X86_64_DEBUG_EXIT_H

#include <stdint.h>

/* I/O port of QEMU's isa-debug-exit device (-device isa-debug-exit,iobase=0xf4,iosize=0x04). */
#define DEBUG_EXIT_PORT 0xf4

/* Write `value` to the debug-exit port. Under QEMU with the device present the
 * emulator exits with status (value << 1) | 1; on real hardware the write goes
 * to an unused port and returns. Used only by kernel test mode. */
void hal_debug_exit(uint32_t value);

#endif /* ARCHOS_ARCH_X86_64_DEBUG_EXIT_H */
