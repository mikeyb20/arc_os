#ifndef ARCHOS_ARCH_X86_64_PIT_H
#define ARCHOS_ARCH_X86_64_PIT_H

#include <stdint.h>

/* PIT I/O ports */
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

/* PIT command register bit fields */
#define PIT_CMD_CHANNEL0   0x00  /* Select channel 0 */
#define PIT_CMD_LOHI       0x30  /* Access mode: lobyte/hibyte */
#define PIT_CMD_MODE0      0x00  /* Mode 0: interrupt on terminal count */

/* PIT base frequency: 1193182 Hz */
#define PIT_BASE_FREQ 1193182

/* Stop channel 0 (left running by firmware, or by an older kernel): the
 * PIT never interrupts once the clock events are up (M1-06). */
void pit_stop(void);

/* TSC cycles that pass while PIT channel 2 counts down `ms` milliseconds
 * (1-54 ms), polled. Returns 0 if the count hasn't ended after max_cycles TSC
 * cycles (PIT missing or clock-gated). Boot-time calibration only (M1-04). */
uint64_t pit_measure_tsc(uint32_t ms, uint64_t max_cycles);

#endif /* ARCHOS_ARCH_X86_64_PIT_H */
