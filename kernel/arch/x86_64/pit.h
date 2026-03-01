#ifndef ARCHOS_ARCH_X86_64_PIT_H
#define ARCHOS_ARCH_X86_64_PIT_H

#include <stdint.h>

/* PIT I/O ports */
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

/* PIT base frequency: 1193182 Hz */
#define PIT_BASE_FREQ 1193182

/* Initialize PIT channel 0 as periodic timer at the given frequency (Hz). */
void pit_init(uint32_t freq_hz);

/* Get total tick count since PIT initialization. */
uint64_t pit_get_ticks(void);

/* Get approximate uptime in milliseconds. */
uint64_t pit_get_uptime_ms(void);

#endif /* ARCHOS_ARCH_X86_64_PIT_H */
