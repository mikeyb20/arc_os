#ifndef ARCHOS_ARCH_X86_64_PIT_H
#define ARCHOS_ARCH_X86_64_PIT_H

#include <stdint.h>

/* PIT I/O ports */
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

/* PIT command register bit fields */
#define PIT_CMD_CHANNEL0   0x00  /* Select channel 0 */
#define PIT_CMD_LOHI       0x30  /* Access mode: lobyte/hibyte */
#define PIT_CMD_MODE2      0x04  /* Mode 2: rate generator */

/* PIT base frequency: 1193182 Hz */
#define PIT_BASE_FREQ 1193182

/* Initialize PIT channel 0 as periodic timer at the given frequency (Hz). */
void pit_init(uint32_t freq_hz);

/* Get total tick count since PIT initialization. */
uint64_t pit_get_ticks(void);

/* Get approximate uptime in milliseconds. */
uint64_t pit_get_uptime_ms(void);

/* TSC cycles that pass while PIT channel 2 counts down `ms` milliseconds
 * (1-54 ms), polled. Returns 0 if the count hasn't ended after max_cycles TSC
 * cycles (PIT missing or clock-gated). Boot-time calibration only (M1-04). */
uint64_t pit_measure_tsc(uint32_t ms, uint64_t max_cycles);

#endif /* ARCHOS_ARCH_X86_64_PIT_H */
